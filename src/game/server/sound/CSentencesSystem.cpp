/***
 *
 *	Copyright (c) 1996-2001, Valve LLC. All rights reserved.
 *
 *	This product contains software technology licensed from Id
 *	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
 *	All Rights Reserved.
 *
 *   Use, distribution, and modification of this source code and/or resulting
 *   object code is restricted to non-commercial enhancements to products from
 *   Valve LLC.  All other use, distribution, or modification is prohibited
 *   without written permission from Valve LLC.
 *
 ****/

#include <fmt/format.h>

#include "cbase.h"
#include "CSentencesSystem.h"
#include "CServerLibrary.h"
#include "sound/sentence_utils.h"

namespace sentences
{
bool CSentencesSystem::Initialize()
{
	m_Logger = g_Logging.CreateLogger("sentences");
	return true;
}

void CSentencesSystem::PostInitialize()
{
	LoadSentences();
}

void CSentencesSystem::Shutdown()
{
	g_Logging.RemoveLogger(m_Logger);
	m_Logger.reset();
}

const char* CSentencesSystem::GetSentenceNameByIndex(int index) const
{
	if (index < 0 || static_cast<std::size_t>(index) >= m_SentenceNames.size())
	{
		ASSERT(!"Invalid index passed to CSentencesSystem::GetSentenceNameByIndex");
		return nullptr;
	}

	return m_SentenceNames[index].c_str();
}

int CSentencesSystem::GetGroupIndex(const char* szgroupname) const
{
	if (!szgroupname)
		return -1;

	// search rgsentenceg for match on szgroupname
	int i = 0;

	for (const auto& group : m_SentenceGroups)
	{
		if (szgroupname == m_SentenceGroups[i].GroupName)
			return i;
		++i;
	}

	return -1;
}

int CSentencesSystem::LookupSentence(const char* sample, SentenceIndexName* sentencenum) const
{
	// this is a sentence name; lookup sentence number
	// and give to engine as string.

	// Skip ! prefix
	++sample;

	// Handle sentence replacement.
	sample = CheckForSentenceReplacement(sample);

	for (size_t i = 0; i < m_SentenceNames.size(); i++)
	{
		if (!stricmp(m_SentenceNames[i].c_str(), sample))
		{
			if (sentencenum)
			{
				fmt::format_to(std::back_inserter(*sentencenum), "!{}", i);
			}
			return static_cast<int>(i);
		}
	}

	// sentence name not found!
	return -1;
}

int CSentencesSystem::PlayRndI(edict_t* entity, int isentenceg, float volume, float attenuation, int flags, int pitch)
{
	SentenceIndexName name;

	const int ipick = Pick(isentenceg, name);
	if (ipick > 0 && !name.empty())
		EMIT_SOUND_DYN(entity, CHAN_VOICE, name.c_str(), volume, attenuation, flags, pitch);
	return ipick;
}

int CSentencesSystem::PlayRndSz(edict_t* entity, const char* szgroupname,
	float volume, float attenuation, int flags, int pitch)
{
	const int isentenceg = GetGroupIndex(szgroupname);
	if (isentenceg < 0)
	{
		ALERT(at_console, "No such sentence group %s\n", szgroupname);
		return -1;
	}

	SentenceIndexName name;

	const int ipick = Pick(isentenceg, name);
	if (ipick >= 0 && !name.empty())
		EMIT_SOUND_DYN(entity, CHAN_VOICE, name.c_str(), volume, attenuation, flags, pitch);

	return ipick;
}

int CSentencesSystem::PlaySequentialSz(edict_t* entity, const char* szgroupname,
	float volume, float attenuation, int flags, int pitch, int ipick, bool freset)
{
	const int isentenceg = GetGroupIndex(szgroupname);
	if (isentenceg < 0)
		return -1;

	SentenceIndexName name;

	const int ipicknext = PickSequential(isentenceg, name, ipick, freset);
	if (ipicknext >= 0 && !name.empty())
		EMIT_SOUND_DYN(entity, CHAN_VOICE, name.c_str(), volume, attenuation, flags, pitch);
	return ipicknext;
}

void CSentencesSystem::Stop(edict_t* entity, int isentenceg, int ipick)
{
	if (isentenceg < 0 || ipick < 0)
		return;

	SentenceIndexName name;
	fmt::format_to(std::back_inserter(name), "!{}{}", m_SentenceGroups[isentenceg].GroupName.c_str(), ipick);

	STOP_SOUND(entity, CHAN_VOICE, name.c_str());
}

void CSentencesSystem::LoadSentences()
{
	m_Logger->trace("Loading sentences.txt");

	m_SentenceNames.clear();
	m_SentenceGroups.clear();

	m_SentenceNames.reserve(InitialSentencesReserveCount);

	const auto fileContents = FileSystem_LoadFileIntoBuffer("sound/sentences.txt", FileContentFormat::Text);

	if (fileContents.empty())
	{
		m_Logger->debug("No sentences to load, file does not exist or is empty");
		return;
	}

	// for each line in the file...
	SentencesListParser parser{{reinterpret_cast<const char*>(fileContents.data())}, *m_Logger};

	for (auto sentence = parser.Next(); sentence; sentence = parser.Next())
	{
		const std::string_view name = std::get<0>(*sentence);

		if (m_SentenceNames.size() >= MaxSentencesCount)
		{
			m_Logger->error("Too many sentences in sentences.txt!");
			break;
		}

		m_SentenceNames.push_back(SentenceName{name.data(), name.size()});

		const auto groupData = ParseGroupData(name);

		if (!groupData)
		{
			continue;
		}

		const std::string_view groupName = std::get<0>(*groupData);

		// if new name doesn't match previous group name,
		// make a new group.

		if (!m_SentenceGroups.empty() && m_SentenceGroups.back().GroupName.c_str() == groupName)
		{
			// name matches with previous, increment group count
			++m_SentenceGroups.back().count;
		}
		else
		{
			// name doesn't match with prev name,
			// copy name into group, init count to 1

			SENTENCEG group;

			group.GroupName.assign(groupName.data(), groupName.size());

			m_SentenceGroups.push_back(group);
		}
	}

	m_Logger->debug("Loaded {} sentences with {} sentence groups", m_SentenceNames.size(), m_SentenceGroups.size());

	// init lru lists
	for (auto& group : m_SentenceGroups)
	{
		InitLRU(group.rgblru, group.count);
	}
}

void CSentencesSystem::InitLRU(unsigned char* plru, int count) const
{
	count = std::min(count, sentences::CSENTENCE_LRU_MAX);

	for (int i = 0; i < count; ++i)
		plru[i] = (unsigned char)i;

	// randomize array
	for (int i = 0; i < (count * 4); i++)
	{
		const int j = RANDOM_LONG(0, count - 1);
		const int k = RANDOM_LONG(0, count - 1);
		std::swap(plru[j], plru[k]);
	}
}

int CSentencesSystem::Pick(int isentenceg, SentenceIndexName& found)
{
	if (isentenceg < 0)
		return -1;

	auto& group = m_SentenceGroups[isentenceg];

	// Make 2 attempts to find a sentence: if we don't find it the first time, reset the LRU array and try again.
	for (int iteration = 0; iteration < 2; ++iteration)
	{
		for (unsigned char i = 0; i < group.count; ++i)
		{
			if (group.rgblru[i] != 0xFF)
			{
				const int ipick = group.rgblru[i];
				group.rgblru[i] = 0xFF;
				
				found.clear();
				fmt::format_to(std::back_inserter(found), "!{}{}", group.GroupName.c_str(), ipick);
				return ipick;
			}
		}

		InitLRU(group.rgblru, group.count);
	}

	return -1;
}

int CSentencesSystem::PickSequential(int isentenceg, SentenceIndexName& found, int ipick, bool freset) const
{
	if (isentenceg < 0)
		return -1;

	const auto& group = m_SentenceGroups[isentenceg];

	if (group.count == 0)
		return -1;

	if (ipick >= group.count)
		ipick = group.count - 1;

	found.clear();
	fmt::format_to(std::back_inserter(found), "!{}{}", group.GroupName.c_str(), ipick);

	if (ipick >= group.count)
	{
		if (freset)
			// reset at end of list
			return 0;
		else
			return group.count;
	}

	return ipick + 1;
}

const char* CSentencesSystem::CheckForSentenceReplacement(const char* sentenceName) const
{
	const auto& map = g_Server.GetMapState()->m_GlobalSentenceReplacement;

	if (auto it = map.find(sentenceName); it != map.end())
	{
		sentenceName = it->second.c_str();
	}

	return sentenceName;
}
}