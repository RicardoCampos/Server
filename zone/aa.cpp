/*	EQEMu: Everquest Server Emulator
Copyright (C) 2001-2004 EQEMu Development Team (http://eqemulator.net)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "../common/classes.h"
#include "../common/global_define.h"
#include "../common/eqemu_logsys.h"
#include "../common/eq_packet_structs.h"
#include "../common/races.h"
#include "../common/spdat.h"
#include "../common/string_util.h"

#include "aa.h"
#include "client.h"
#include "corpse.h"
#include "groups.h"
#include "mob.h"
#include "queryserv.h"
#include "raids.h"
#include "string_ids.h"
#include "titles.h"
#include "zonedb.h"

extern QueryServ* QServ;


AA_DBAction AA_Actions[aaHighestID][MAX_AA_ACTION_RANKS];	//[aaid][rank]
std::map<uint32,SendAA_Struct*>aas_send;
std::map<uint32, std::map<uint32, AA_Ability> > aa_effects;	//stores the effects from the aa_effects table in memory
std::map<uint32, AALevelCost_Struct> AARequiredLevelAndCost;


int Client::GetAATimerID(aaID activate)
{
	SendAA_Struct* aa2 = zone->FindAA(activate);

	if(!aa2)
	{
		for(int i = 1;i < MAX_AA_ACTION_RANKS; ++i)
		{
			int a = activate - i;

			if(a <= 0)
				break;

			aa2 = zone->FindAA(a);

			if(aa2 != nullptr)
				break;
		}
	}

	if(aa2)
		return aa2->spell_type;

	return 0;
}

int Client::CalcAAReuseTimer(const AA_DBAction *caa) {

	if(!caa)
		return 0;

	int ReuseTime = caa->reuse_time;

	if(ReuseTime > 0)
	{
		int ReductionPercentage;

		if(caa->redux_aa > 0 && caa->redux_aa < aaHighestID)
		{
			ReductionPercentage = GetAA(caa->redux_aa) * caa->redux_rate;

			if(caa->redux_aa2 > 0 && caa->redux_aa2 < aaHighestID)
				ReductionPercentage += (GetAA(caa->redux_aa2) * caa->redux_rate2);

			ReuseTime = caa->reuse_time * (100 - ReductionPercentage) / 100;
		}

	}
	return ReuseTime;
}

void Client::ActivateAA(aaID activate){
	if(activate < 0 || activate >= aaHighestID)
		return;
	if(IsStunned() || IsFeared() || IsMezzed() || IsSilenced() || IsPet() || IsSitting() || GetFeigned())
		return;

	int AATimerID = GetAATimerID(activate);

	SendAA_Struct* aa2 = nullptr;
	aaID aaid = activate;
	uint8 activate_val = GetAA(activate);
	//this wasn't taking into acct multi tiered act talents before...
	if(activate_val == 0){
		aa2 = zone->FindAA(activate);
		if(!aa2){
			int i;
			int a;
			for(i=1;i<MAX_AA_ACTION_RANKS;i++){
				a = activate - i;
				if(a <= 0)
					break;

				aa2 = zone->FindAA(a);
				if(aa2 != nullptr)
					break;
			}
		}
		if(aa2){
			aaid = (aaID) aa2->id;
			activate_val = GetAA(aa2->id);
		}
	}

	if (activate_val == 0){
		return;
	}

	if(aa2)
	{
		if(aa2->account_time_required)
		{
			if((Timer::GetTimeSeconds() + account_creation) < aa2->account_time_required)
			{
				return;
			}
		}
	}

	if(!p_timers.Expired(&database, AATimerID + pTimerAAStart))
	{
		uint32 aaremain = p_timers.GetRemainingTime(AATimerID + pTimerAAStart);
		uint32 aaremain_hr = aaremain / (60 * 60);
		uint32 aaremain_min = (aaremain / 60) % 60;
		uint32 aaremain_sec = aaremain % 60;

		if(aa2) {
			if (aaremain_hr >= 1)	//1 hour or more
				Message(13, "You can use the ability %s again in %u hour(s) %u minute(s) %u seconds",
				aa2->name, aaremain_hr, aaremain_min, aaremain_sec);
			else	//less than an hour
				Message(13, "You can use the ability %s again in %u minute(s) %u seconds",
				aa2->name, aaremain_min, aaremain_sec);
		} else {
			if (aaremain_hr >= 1)	//1 hour or more
				Message(13, "You can use this ability again in %u hour(s) %u minute(s) %u seconds",
				aaremain_hr, aaremain_min, aaremain_sec);
			else	//less than an hour
				Message(13, "You can use this ability again in %u minute(s) %u seconds",
				aaremain_min, aaremain_sec);
		}
		return;
	}

	if(activate_val > MAX_AA_ACTION_RANKS)
		activate_val = MAX_AA_ACTION_RANKS;
	activate_val--;		//to get array index.

	//get our current node, now that the indices are well bounded
	const AA_DBAction *caa = &AA_Actions[aaid][activate_val];

	if((aaid == aaImprovedHarmTouch || aaid == aaLeechTouch) && !p_timers.Expired(&database, pTimerHarmTouch)){
		Message(13,"Ability recovery time not yet met.");
		return;
	}

	//everything should be configured out now

	uint16 target_id = 0;

	//figure out our target
	switch(caa->target) {
		case aaTargetUser:
		case aaTargetGroup:
			target_id = GetID();
			break;
		case aaTargetCurrent:
		case aaTargetCurrentGroup:
			if(GetTarget() == nullptr) {
				Message_StringID(MT_DefaultText, AA_NO_TARGET);	//You must first select a target for this ability!
				p_timers.Clear(&database, AATimerID + pTimerAAStart);
				return;
			}
			target_id = GetTarget()->GetID();
			break;
		case aaTargetPet:
			if(GetPet() == nullptr) {
				Message(0, "A pet is required for this skill.");
				return;
			}
			target_id = GetPetID();
			break;
	}

	//handle non-spell action
	if(caa->action != aaActionNone) {
		if(caa->mana_cost > 0) {
			if(GetMana() < caa->mana_cost) {
				Message_StringID(13, INSUFFICIENT_MANA);
				return;
			}
			SetMana(GetMana() - caa->mana_cost);
		}
		if(caa->reuse_time > 0)
		{
			uint32 timer_base = CalcAAReuseTimer(caa);
			if(activate == aaImprovedHarmTouch || activate == aaLeechTouch)
			{
				p_timers.Start(pTimerHarmTouch, HarmTouchReuseTime);
			}
			p_timers.Start(AATimerID + pTimerAAStart, timer_base);
			SendAATimer(AATimerID, 0, 0);
		}
		HandleAAAction(aaid);
	}

	//cast the spell, if we have one
	if(caa->spell_id > 0 && caa->spell_id < SPDAT_RECORDS) {

		if(caa->reuse_time > 0)
		{
			uint32 timer_base = CalcAAReuseTimer(caa);
			SendAATimer(AATimerID, 0, 0);
			p_timers.Start(AATimerID + pTimerAAStart, timer_base);
			if(activate == aaImprovedHarmTouch || activate == aaLeechTouch)
			{
				p_timers.Start(pTimerHarmTouch, HarmTouchReuseTime);
			}
			// Bards can cast instant cast AAs while they are casting another song
			if (spells[caa->spell_id].cast_time == 0 && GetClass() == BARD && IsBardSong(casting_spell_id)) {
				if(!SpellFinished(caa->spell_id, entity_list.GetMob(target_id), 10, -1, -1, spells[caa->spell_id].ResistDiff, false)) {
					//Reset on failed cast
					SendAATimer(AATimerID, 0, 0xFFFFFF);
					Message_StringID(15,ABILITY_FAILED);
					p_timers.Clear(&database, AATimerID + pTimerAAStart);
					return;
				}
			} else {
				if (!CastSpell(caa->spell_id, target_id, USE_ITEM_SPELL_SLOT, -1, -1, 0, -1, AATimerID + pTimerAAStart, timer_base, 1)) {
					//Reset on failed cast
					SendAATimer(AATimerID, 0, 0xFFFFFF);
					Message_StringID(15,ABILITY_FAILED);
					p_timers.Clear(&database, AATimerID + pTimerAAStart);
					return;
				}
			}
		}
		else
		{
			if(!CastSpell(caa->spell_id, target_id))
				return;
		}
	}
	// Check if AA is expendable
	if (aas_send[activate - activate_val]->special_category == 7) {

		// Add the AA cost to the extended profile to track overall total
		m_epp.expended_aa += aas_send[activate]->cost;

		SetAA(activate, 0);

		SaveAA(); /* Save Character AA */
		SendAA(activate);
		SendAATable();
	}
}

void Client::HandleAAAction(aaID activate) {
	if(activate < 0 || activate >= aaHighestID)
		return;

	uint8 activate_val = GetAA(activate);

	if (activate_val == 0)
		return;

	if(activate_val > MAX_AA_ACTION_RANKS)
		activate_val = MAX_AA_ACTION_RANKS;
	activate_val--;		//to get array index.

	//get our current node, now that the indices are well bounded
	const AA_DBAction *caa = &AA_Actions[activate][activate_val];

	uint16 timer_id = 0;
	uint16 timer_duration = caa->duration;
	aaTargetType target = aaTargetUser;

	uint16 spell_id = SPELL_UNKNOWN;	//gets cast at the end if not still unknown

	switch(caa->action) {
		case aaActionAETaunt:
			entity_list.AETaunt(this);
			break;

		case aaActionFlamingArrows:
			//toggle it
			if(CheckAAEffect(aaEffectFlamingArrows))
				EnableAAEffect(aaEffectFlamingArrows);
			else
				DisableAAEffect(aaEffectFlamingArrows);
			break;

		case aaActionFrostArrows:
			if(CheckAAEffect(aaEffectFrostArrows))
				EnableAAEffect(aaEffectFrostArrows);
			else
				DisableAAEffect(aaEffectFrostArrows);
			break;

		case aaActionRampage:
			EnableAAEffect(aaEffectRampage, 10);
			break;

		case aaActionSharedHealth:
			if(CheckAAEffect(aaEffectSharedHealth))
				EnableAAEffect(aaEffectSharedHealth);
			else
				DisableAAEffect(aaEffectSharedHealth);
			break;

		case aaActionCelestialRegen: {
			//special because spell_id depends on a different AA
			switch (GetAA(aaCelestialRenewal)) {
				case 1:
					spell_id = 3250;
					break;
				case 2:
					spell_id = 3251;
					break;
				default:
					spell_id = 2740;
					break;
			}
			target = aaTargetCurrent;
			break;
		}

		case aaActionDireCharm: {
			//special because spell_id depends on class
			switch (GetClass())
			{
				case DRUID:
					spell_id = 2760;	//2644?
					break;
				case NECROMANCER:
					spell_id = 2759;	//2643?
					break;
				case ENCHANTER:
					spell_id = 2761;	//2642?
					break;
			}
			target = aaTargetCurrent;
			break;
		}

		case aaActionImprovedFamiliar: {
			//Spell IDs might be wrong...
			if (GetAA(aaAllegiantFamiliar))
				spell_id = 3264;	//1994?
			else
				spell_id = 2758;	//2155?
			break;
		}

		case aaActionActOfValor:
			if(GetTarget() != nullptr) {
				int curhp = GetTarget()->GetHP();
				target = aaTargetCurrent;
				GetTarget()->HealDamage(curhp, this);
				Death(this, 0, SPELL_UNKNOWN, SkillHandtoHand);
			}
			break;

		case aaActionSuspendedMinion:
			if (GetPet()) {
				target = aaTargetPet;
				switch (GetAA(aaSuspendedMinion)) {
					case 1:
						spell_id = 3248;
						break;
					case 2:
						spell_id = 3249;
						break;
				}
				//do we really need to cast a spell?

				Message(0,"You call your pet to your side.");
				GetPet()->WipeHateList();
				GetPet()->GMMove(GetX(),GetY(),GetZ());
				if (activate_val > 1)
					entity_list.ClearFeignAggro(GetPet());
			} else {
				Message(0,"You have no pet to call.");
			}
			break;

		case aaActionEscape:
			Escape();
			break;

		// Don't think this code is used any longer for Bestial Alignment as the aa.has a spell_id and no nonspell_action.
		case aaActionBeastialAlignment:
			switch(GetBaseRace()) {
				case BARBARIAN:
					spell_id = AA_Choose3(activate_val, 4521, 4522, 4523);
					break;
				case TROLL:
					spell_id = AA_Choose3(activate_val, 4524, 4525, 4526);
					break;
				case OGRE:
					spell_id = AA_Choose3(activate_val, 4527, 4527, 4529);
					break;
				case IKSAR:
					spell_id = AA_Choose3(activate_val, 4530, 4531, 4532);
					break;
				case VAHSHIR:
					spell_id = AA_Choose3(activate_val, 4533, 4534, 4535);
					break;
			}

		case aaActionLeechTouch:
			target = aaTargetCurrent;
			spell_id = SPELL_HARM_TOUCH2;
			EnableAAEffect(aaEffectLeechTouch, 1000);
			break;

		case aaActionFadingMemories:
			// Do nothing since spell effect works correctly, but mana isn't used.
			break;

		default:
			Log.Out(Logs::General, Logs::Error, "Unknown AA nonspell action type %d", caa->action);
			return;
	}


	uint16 target_id = 0;
	//figure out our target
	switch(target) {
		case aaTargetUser:
		case aaTargetGroup:
			target_id = GetID();
			break;
		case aaTargetCurrent:
		case aaTargetCurrentGroup:
			if(GetTarget() == nullptr) {
				Message_StringID(MT_DefaultText, AA_NO_TARGET);	//You must first select a target for this ability!
				p_timers.Clear(&database, timer_id + pTimerAAEffectStart);
				return;
			}
			target_id = GetTarget()->GetID();
			break;
		case aaTargetPet:
			if(GetPet() == nullptr) {
				Message(0, "A pet is required for this skill.");
				return;
			}
			target_id = GetPetID();
			break;
	}

	//cast the spell, if we have one
	if(IsValidSpell(spell_id)) {
		int aatid = GetAATimerID(activate);
		if (!CastSpell(spell_id, target_id, USE_ITEM_SPELL_SLOT, -1, -1, 0, -1, pTimerAAStart + aatid, CalcAAReuseTimer(caa), 1)) {
			SendAATimer(aatid, 0, 0xFFFFFF);
			Message_StringID(15,ABILITY_FAILED);
			p_timers.Clear(&database, pTimerAAStart + aatid);
			return;
		}
	}

	//handle the duration timer if we have one.
	if(timer_id > 0 && timer_duration > 0) {
		p_timers.Start(pTimerAAEffectStart + timer_id, timer_duration);
	}
}

void Mob::TemporaryPets(uint16 spell_id, Mob *targ, const char *name_override, uint32 duration_override, bool followme, bool sticktarg) {

	//It might not be a bad idea to put these into the database, eventually..

	//Dook- swarms and wards

	PetRecord record;
	if(!database.GetPetEntry(spells[spell_id].teleport_zone, &record))
	{
		Log.Out(Logs::General, Logs::Error, "Unknown swarm pet spell id: %d, check pets table", spell_id);
		Message(13, "Unable to find data for pet %s", spells[spell_id].teleport_zone);
		return;
	}

	AA_SwarmPet pet;
	pet.count = 1;
	pet.duration = 1;

	for(int x = 0; x < MAX_SWARM_PETS; x++)
	{
		if(spells[spell_id].effectid[x] == SE_TemporaryPets)
		{
			pet.count = spells[spell_id].base[x];
			pet.duration = spells[spell_id].max[x];
		}
	}

	pet.duration += GetFocusEffect(focusSwarmPetDuration, spell_id) / 1000;

	pet.npc_id = record.npc_type;

	NPCType *made_npc = nullptr;

	const NPCType *npc_type = database.GetNPCType(pet.npc_id);
	if(npc_type == nullptr) {
		//log write
		Log.Out(Logs::General, Logs::Error, "Unknown npc type for swarm pet spell id: %d", spell_id);
		Message(0,"Unable to find pet!");
		return;
	}

	if(name_override != nullptr) {
		//we have to make a custom NPC type for this name change
		made_npc = new NPCType;
		memcpy(made_npc, npc_type, sizeof(NPCType));
		strcpy(made_npc->name, name_override);
		npc_type = made_npc;
	}

	int summon_count = 0;
	summon_count = pet.count;

	if(summon_count > MAX_SWARM_PETS)
		summon_count = MAX_SWARM_PETS;

	static const glm::vec2 swarmPetLocations[MAX_SWARM_PETS] = {
		glm::vec2(5, 5), glm::vec2(-5, 5), glm::vec2(5, -5), glm::vec2(-5, -5),
		glm::vec2(10, 10), glm::vec2(-10, 10), glm::vec2(10, -10), glm::vec2(-10, -10),
		glm::vec2(8, 8), glm::vec2(-8, 8), glm::vec2(8, -8), glm::vec2(-8, -8)
	};

	while(summon_count > 0) {
		int pet_duration = pet.duration;
		if(duration_override > 0)
			pet_duration = duration_override;

		//this is a little messy, but the only way to do it right
		//it would be possible to optimize out this copy for the last pet, but oh well
		NPCType *npc_dup = nullptr;
		if(made_npc != nullptr) {
			npc_dup = new NPCType;
			memcpy(npc_dup, made_npc, sizeof(NPCType));
		}

		NPC* npca = new NPC(
				(npc_dup!=nullptr)?npc_dup:npc_type,	//make sure we give the NPC the correct data pointer
				0,
				GetPosition() + glm::vec4(swarmPetLocations[summon_count], 0.0f, 0.0f),
				FlyMode3);

		if (followme)
			npca->SetFollowID(GetID());

		if(!npca->GetSwarmInfo()){
			AA_SwarmPetInfo* nSI = new AA_SwarmPetInfo;
			npca->SetSwarmInfo(nSI);
			npca->GetSwarmInfo()->duration = new Timer(pet_duration*1000);
		}
		else{
			npca->GetSwarmInfo()->duration->Start(pet_duration*1000);
		}

		//removing this prevents the pet from attacking
		npca->GetSwarmInfo()->owner_id = GetID();

		//give the pets somebody to "love"
		if(targ != nullptr){
			npca->AddToHateList(targ, 1000, 1000);
			if (RuleB(Spells, SwarmPetTargetLock) || sticktarg)
				npca->GetSwarmInfo()->target = targ->GetID();
			else
				npca->GetSwarmInfo()->target = 0;
		}

		//we allocated a new NPC type object, give the NPC ownership of that memory
		if(npc_dup != nullptr)
			npca->GiveNPCTypeData(npc_dup);

		entity_list.AddNPC(npca, true, true);
		summon_count--;
	}

	//the target of these swarm pets will take offense to being cast on...
	if(targ != nullptr)
		targ->AddToHateList(this, 1, 0);

	// The other pointers we make are handled elsewhere.
	delete made_npc;
}

void Mob::TypesTemporaryPets(uint32 typesid, Mob *targ, const char *name_override, uint32 duration_override, bool followme, bool sticktarg) {

	AA_SwarmPet pet;
	pet.count = 1;
	pet.duration = 1;

	pet.npc_id = typesid;

	NPCType *made_npc = nullptr;

	const NPCType *npc_type = database.GetNPCType(typesid);
	if(npc_type == nullptr) {
		//log write
		Log.Out(Logs::General, Logs::Error, "Unknown npc type for swarm pet type id: %d", typesid);
		Message(0,"Unable to find pet!");
		return;
	}

	if(name_override != nullptr) {
		//we have to make a custom NPC type for this name change
		made_npc = new NPCType;
		memcpy(made_npc, npc_type, sizeof(NPCType));
		strcpy(made_npc->name, name_override);
		npc_type = made_npc;
	}

	int summon_count = 0;
	summon_count = pet.count;

	if(summon_count > MAX_SWARM_PETS)
		summon_count = MAX_SWARM_PETS;

	static const glm::vec2 swarmPetLocations[MAX_SWARM_PETS] = {
		glm::vec2(5, 5), glm::vec2(-5, 5), glm::vec2(5, -5), glm::vec2(-5, -5),
		glm::vec2(10, 10), glm::vec2(-10, 10), glm::vec2(10, -10), glm::vec2(-10, -10),
		glm::vec2(8, 8), glm::vec2(-8, 8), glm::vec2(8, -8), glm::vec2(-8, -8)
	};;

	while(summon_count > 0) {
		int pet_duration = pet.duration;
		if(duration_override > 0)
			pet_duration = duration_override;

		//this is a little messy, but the only way to do it right
		//it would be possible to optimize out this copy for the last pet, but oh well
		NPCType *npc_dup = nullptr;
		if(made_npc != nullptr) {
			npc_dup = new NPCType;
			memcpy(npc_dup, made_npc, sizeof(NPCType));
		}

		NPC* npca = new NPC(
				(npc_dup!=nullptr)?npc_dup:npc_type,	//make sure we give the NPC the correct data pointer
				0,
				GetPosition() + glm::vec4(swarmPetLocations[summon_count], 0.0f, 0.0f),
				FlyMode3);

		if (followme)
			npca->SetFollowID(GetID());

		if(!npca->GetSwarmInfo()){
			AA_SwarmPetInfo* nSI = new AA_SwarmPetInfo;
			npca->SetSwarmInfo(nSI);
			npca->GetSwarmInfo()->duration = new Timer(pet_duration*1000);
		}
		else{
			npca->GetSwarmInfo()->duration->Start(pet_duration*1000);
		}

		//removing this prevents the pet from attacking
		npca->GetSwarmInfo()->owner_id = GetID();

		//give the pets somebody to "love"
		if(targ != nullptr){
			npca->AddToHateList(targ, 1000, 1000);

			if (RuleB(Spells, SwarmPetTargetLock) || sticktarg)
				npca->GetSwarmInfo()->target = targ->GetID();
			else
				npca->GetSwarmInfo()->target = 0;
		}

		//we allocated a new NPC type object, give the NPC ownership of that memory
		if(npc_dup != nullptr)
			npca->GiveNPCTypeData(npc_dup);

		entity_list.AddNPC(npca, true, true);
		summon_count--;
	}

	// The other pointers we make are handled elsewhere.
	delete made_npc;
}

void Mob::WakeTheDead(uint16 spell_id, Mob *target, uint32 duration)
{
	Corpse *CorpseToUse = nullptr;
	CorpseToUse = entity_list.GetClosestCorpse(this, nullptr);

	if(!CorpseToUse)
		return;

	//assuming we have pets in our table; we take the first pet as a base type.
	const NPCType *base_type = database.GetNPCType(500);
	NPCType *make_npc = new NPCType;
	memcpy(make_npc, base_type, sizeof(NPCType));

	//combat stats
	make_npc->AC = ((GetLevel() * 7) + 550);
	make_npc->ATK = GetLevel();
	make_npc->max_dmg = (GetLevel() * 4) + 2;
	make_npc->min_dmg = 1;

	//base stats
	make_npc->cur_hp = (GetLevel() * 55);
	make_npc->max_hp = (GetLevel() * 55);
	make_npc->STR = 85 + (GetLevel() * 3);
	make_npc->STA = 85 + (GetLevel() * 3);
	make_npc->DEX = 85 + (GetLevel() * 3);
	make_npc->AGI = 85 + (GetLevel() * 3);
	make_npc->INT = 85 + (GetLevel() * 3);
	make_npc->WIS = 85 + (GetLevel() * 3);
	make_npc->CHA = 85 + (GetLevel() * 3);
	make_npc->MR = 25;
	make_npc->FR = 25;
	make_npc->CR = 25;
	make_npc->DR = 25;
	make_npc->PR = 25;

	//level class and gender
	make_npc->level = GetLevel();
	make_npc->class_ = CorpseToUse->class_;
	make_npc->race = CorpseToUse->race;
	make_npc->gender = CorpseToUse->gender;
	make_npc->loottable_id = 0;
	//name
	char NewName[64];
	sprintf(NewName, "%s`s Animated Corpse", GetCleanName());
	strcpy(make_npc->name, NewName);

	//appearance
	make_npc->beard = CorpseToUse->beard;
	make_npc->beardcolor = CorpseToUse->beardcolor;
	make_npc->eyecolor1 = CorpseToUse->eyecolor1;
	make_npc->eyecolor2 = CorpseToUse->eyecolor2;
	make_npc->haircolor = CorpseToUse->haircolor;
	make_npc->hairstyle = CorpseToUse->hairstyle;
	make_npc->helmtexture = CorpseToUse->helmtexture;
	make_npc->luclinface = CorpseToUse->luclinface;
	make_npc->size = CorpseToUse->size;
	make_npc->texture = CorpseToUse->texture;

	//cast stuff.. based off of PEQ's if you want to change
	//it you'll have to mod this code, but most likely
	//most people will be using PEQ style for the first
	//part of their spell list; can't think of any smooth
	//way to do this
	//some basic combat mods here too since it's convienent
	switch(CorpseToUse->class_)
	{
	case CLERIC:
		make_npc->npc_spells_id = 1;
		break;
	case WIZARD:
		make_npc->npc_spells_id = 2;
		break;
	case NECROMANCER:
		make_npc->npc_spells_id = 3;
		break;
	case MAGICIAN:
		make_npc->npc_spells_id = 4;
		break;
	case ENCHANTER:
		make_npc->npc_spells_id = 5;
		break;
	case SHAMAN:
		make_npc->npc_spells_id = 6;
		break;
	case DRUID:
		make_npc->npc_spells_id = 7;
		break;
	case PALADIN:
		//SPECATK_TRIPLE
		strcpy(make_npc->special_abilities, "6,1");
		make_npc->cur_hp = make_npc->cur_hp * 150 / 100;
		make_npc->max_hp = make_npc->max_hp * 150 / 100;
		make_npc->npc_spells_id = 8;
		break;
	case SHADOWKNIGHT:
		strcpy(make_npc->special_abilities, "6,1");
		make_npc->cur_hp = make_npc->cur_hp * 150 / 100;
		make_npc->max_hp = make_npc->max_hp * 150 / 100;
		make_npc->npc_spells_id = 9;
		break;
	case RANGER:
		strcpy(make_npc->special_abilities, "7,1");
		make_npc->cur_hp = make_npc->cur_hp * 135 / 100;
		make_npc->max_hp = make_npc->max_hp * 135 / 100;
		make_npc->npc_spells_id = 10;
		break;
	case BARD:
		strcpy(make_npc->special_abilities, "6,1");
		make_npc->cur_hp = make_npc->cur_hp * 110 / 100;
		make_npc->max_hp = make_npc->max_hp * 110 / 100;
		make_npc->npc_spells_id = 11;
		break;
	case BEASTLORD:
		strcpy(make_npc->special_abilities, "7,1");
		make_npc->cur_hp = make_npc->cur_hp * 110 / 100;
		make_npc->max_hp = make_npc->max_hp * 110 / 100;
		make_npc->npc_spells_id = 12;
		break;
	case ROGUE:
		strcpy(make_npc->special_abilities, "7,1");
		make_npc->max_dmg = make_npc->max_dmg * 150 /100;
		make_npc->cur_hp = make_npc->cur_hp * 110 / 100;
		make_npc->max_hp = make_npc->max_hp * 110 / 100;
		break;
	case MONK:
		strcpy(make_npc->special_abilities, "7,1");
		make_npc->max_dmg = make_npc->max_dmg * 150 /100;
		make_npc->cur_hp = make_npc->cur_hp * 135 / 100;
		make_npc->max_hp = make_npc->max_hp * 135 / 100;
		break;
	case WARRIOR:
	case BERSERKER:
		strcpy(make_npc->special_abilities, "7,1");
		make_npc->max_dmg = make_npc->max_dmg * 150 /100;
		make_npc->cur_hp = make_npc->cur_hp * 175 / 100;
		make_npc->max_hp = make_npc->max_hp * 175 / 100;
		break;
	default:
		make_npc->npc_spells_id = 0;
		break;
	}

	make_npc->loottable_id = 0;
	make_npc->merchanttype = 0;
	make_npc->d_melee_texture1 = 0;
	make_npc->d_melee_texture2 = 0;

	NPC* npca = new NPC(make_npc, 0, GetPosition(), FlyMode3);

	if(!npca->GetSwarmInfo()){
		AA_SwarmPetInfo* nSI = new AA_SwarmPetInfo;
		npca->SetSwarmInfo(nSI);
		npca->GetSwarmInfo()->duration = new Timer(duration*1000);
	}
	else{
		npca->GetSwarmInfo()->duration->Start(duration*1000);
	}

	npca->GetSwarmInfo()->owner_id = GetID();

	//give the pet somebody to "love"
	if(target != nullptr){
		npca->AddToHateList(target, 100000);
		npca->GetSwarmInfo()->target = target->GetID();
	}

	//gear stuff, need to make sure there's
	//no situation where this stuff can be duped
	for(int x = EmuConstants::EQUIPMENT_BEGIN; x <= EmuConstants::EQUIPMENT_END; x++) // (< 21) added MainAmmo
	{
		uint32 sitem = 0;
		sitem = CorpseToUse->GetWornItem(x);
		if(sitem){
			const Item_Struct * itm = database.GetItem(sitem);
			npca->AddLootDrop(itm, &npca->itemlist, 1, 1, 127, true, true);
		}
	}

	//we allocated a new NPC type object, give the NPC ownership of that memory
	if(make_npc != nullptr)
		npca->GiveNPCTypeData(make_npc);

	entity_list.AddNPC(npca, true, true);

	//the target of these swarm pets will take offense to being cast on...
	if(target != nullptr)
		target->AddToHateList(this, 1, 0);
}

//turn on an AA effect
//duration == 0 means no time limit, used for one-shot deals, etc..
void Client::EnableAAEffect(aaEffectType type, uint32 duration) {
	if(type > _maxaaEffectType)
		return;	//for now, special logic needed.
	m_epp.aa_effects |= 1 << (type-1);

	if(duration > 0) {
		p_timers.Start(pTimerAAEffectStart + type, duration);
	} else {
		p_timers.Clear(&database, pTimerAAEffectStart + type);
	}
}

void Client::DisableAAEffect(aaEffectType type) {
	if(type > _maxaaEffectType)
		return;	//for now, special logic needed.
	uint32 bit = 1 << (type-1);
	if(m_epp.aa_effects & bit) {
		m_epp.aa_effects ^= bit;
	}
	p_timers.Clear(&database, pTimerAAEffectStart + type);
}

/*
By default an AA effect is a one shot deal, unless
a duration timer is set.
*/
bool Client::CheckAAEffect(aaEffectType type) {
	if(type > _maxaaEffectType)
		return(false);	//for now, special logic needed.
	if(m_epp.aa_effects & (1 << (type-1))) {	//is effect enabled?
		//has our timer expired?
		if(p_timers.Expired(&database, pTimerAAEffectStart + type)) {
			DisableAAEffect(type);
			return(false);
		}
		return(true);
	}
	return(false);
}

void Client::SendAAStats() {
	EQApplicationPacket* outapp = new EQApplicationPacket(OP_AAExpUpdate, sizeof(AltAdvStats_Struct));
	AltAdvStats_Struct *aps = (AltAdvStats_Struct *)outapp->pBuffer;
	aps->experience = m_pp.expAA;
	aps->experience = (uint32)(((float)330.0f * (float)m_pp.expAA) / (float)max_AAXP);
	aps->unspent = m_pp.aapoints;
	aps->percentage = m_epp.perAA;
	QueuePacket(outapp);
	safe_delete(outapp);
}

void Client::BuyAA(AA_Action* action)
{
	Log.Out(Logs::Detail, Logs::AA, "Starting to buy AA %d", action->ability);

	//find the AA information from the database
	SendAA_Struct* aa2 = zone->FindAA(action->ability);
	if(!aa2) {
		//hunt for a lower level...
		int i;
		int a;
		for(i=1;i<MAX_AA_ACTION_RANKS;i++){
			a = action->ability - i;
			if(a <= 0)
				break;
			Log.Out(Logs::Detail, Logs::AA, "Could not find AA %d, trying potential parent %d", action->ability, a);
			aa2 = zone->FindAA(a);
			if(aa2 != nullptr)
				break;
		}
	}
	if(aa2 == nullptr)
		return;	//invalid ability...

	if(aa2->special_category == 1 || aa2->special_category == 2)
		return; // Not purchasable progression style AAs

	if(aa2->special_category == 8 && aa2->cost == 0)
		return; // Not purchasable racial AAs(set a cost to make them purchasable)

	uint32 cur_level = GetAA(aa2->id);
	if((aa2->id + cur_level) != action->ability) { //got invalid AA
		Log.Out(Logs::Detail, Logs::AA, "Unable to find or match AA %d (found %d + lvl %d)", action->ability, aa2->id, cur_level);
		return;
	}

	if(aa2->account_time_required)
	{
		if((Timer::GetTimeSeconds() - account_creation) < aa2->account_time_required)
		{
			return;
		}
	}

	uint32 real_cost;
	uint8 req_level;
	std::map<uint32, AALevelCost_Struct>::iterator RequiredLevel = AARequiredLevelAndCost.find(action->ability);

	if(RequiredLevel != AARequiredLevelAndCost.end()) {
		real_cost = RequiredLevel->second.Cost;
		req_level = RequiredLevel->second.Level;
	}
	else {
		real_cost = aa2->cost + (aa2->cost_inc * cur_level);
		req_level = aa2->class_type + (aa2->level_inc * cur_level);
	}

	if (req_level > GetLevel())
		return; //Cheater trying to Buy AA...

	if (m_pp.aapoints >= real_cost && cur_level < aa2->max_level) {
		SetAA(aa2->id, cur_level + 1);

		Log.Out(Logs::Detail, Logs::AA, "Set AA %d to level %d", aa2->id, cur_level + 1);

		m_pp.aapoints -= real_cost;

		/* Do Player Profile rank calculations and set player profile */
		SaveAA();
		/* Save to Database to avoid having to write the whole AA array to the profile, only write changes*/
		// database.SaveCharacterAA(this->CharacterID(), aa2->id, (cur_level + 1));

		if ((RuleB(AA, Stacking) && (GetClientVersionBit() >= 4) && (aa2->hotkey_sid == 4294967295u))
			&& ((aa2->max_level == (cur_level + 1)) && aa2->sof_next_id)){
			SendAA(aa2->id);
			SendAA(aa2->sof_next_id);
		}
		else
			SendAA(aa2->id);

		SendAATable();

		/*
			We are building these messages ourself instead of using the stringID to work around patch discrepencies
				these are AA_GAIN_ABILITY	(410) & AA_IMPROVE (411), respectively, in both Titanium & SoF. not sure about 6.2
		*/

		/* Initial purchase of an AA ability */
		if (cur_level < 1){
			Message(15, "You have gained the ability \"%s\" at a cost of %d ability %s.", aa2->name, real_cost, (real_cost>1) ? "points" : "point");

			/* QS: Player_Log_AA_Purchases */
			if (RuleB(QueryServ, PlayerLogAAPurchases)){
				std::string event_desc = StringFormat("Initial AA Purchase :: aa_name:%s aa_id:%i at cost:%i in zoneid:%i instid:%i", aa2->name, aa2->id, real_cost, this->GetZoneID(), this->GetInstanceID());
				QServ->PlayerLogEvent(Player_Log_AA_Purchases, this->CharacterID(), event_desc);
			}
		}
		/* Ranked purchase of an AA ability */
		else{
			Message(15, "You have improved %s %d at a cost of %d ability %s.", aa2->name, cur_level + 1, real_cost, (real_cost > 1) ? "points" : "point");

			/* QS: Player_Log_AA_Purchases */
			if (RuleB(QueryServ, PlayerLogAAPurchases)){
				std::string event_desc = StringFormat("Ranked AA Purchase :: aa_name:%s aa_id:%i at cost:%i in zoneid:%i instid:%i", aa2->name, aa2->id, real_cost, this->GetZoneID(), this->GetInstanceID());
				QServ->PlayerLogEvent(Player_Log_AA_Purchases, this->CharacterID(), event_desc);
			}
		}

		SendAAStats();

		CalcBonuses();
		if(title_manager.IsNewAATitleAvailable(m_pp.aapoints_spent, GetBaseClass()))
			NotifyNewTitlesAvailable();
	}
}

void Client::SendAATimer(uint32 ability, uint32 begin, uint32 end) {
	EQApplicationPacket* outapp = new EQApplicationPacket(OP_AAAction,sizeof(UseAA_Struct));
	UseAA_Struct* uaaout = (UseAA_Struct*)outapp->pBuffer;
	uaaout->ability = ability;
	uaaout->begin = begin;
	uaaout->end = end;
	QueuePacket(outapp);
	safe_delete(outapp);
}

//sends all AA timers.
void Client::SendAATimers() {
	//we dont use SendAATimer because theres no reason to allocate the EQApplicationPacket every time
	EQApplicationPacket* outapp = new EQApplicationPacket(OP_AAAction,sizeof(UseAA_Struct));
	UseAA_Struct* uaaout = (UseAA_Struct*)outapp->pBuffer;

	PTimerList::iterator c,e;
	c = p_timers.begin();
	e = p_timers.end();
	for(; c != e; ++c) {
		PersistentTimer *cur = c->second;
		if(cur->GetType() < pTimerAAStart || cur->GetType() > pTimerAAEnd)
			continue;	//not an AA timer
		//send timer
		uaaout->begin = cur->GetStartTime();
		uaaout->end = static_cast<uint32>(time(nullptr));
		uaaout->ability = cur->GetType() - pTimerAAStart; // uuaaout->ability is really a shared timer number
		QueuePacket(outapp);
	}

	safe_delete(outapp);
}

void Client::SendAATable() {
	EQApplicationPacket* outapp = new EQApplicationPacket(OP_RespondAA, sizeof(AATable_Struct));

	AATable_Struct* aa2 = (AATable_Struct *)outapp->pBuffer;
	aa2->aa_spent = GetAAPointsSpent();

	uint32 i;
	for(i=0;i < MAX_PP_AA_ARRAY;i++){
		aa2->aa_list[i].aa_skill = aa[i]->AA;
		aa2->aa_list[i].aa_value = aa[i]->value;
		aa2->aa_list[i].unknown08 = 0;
	}
	QueuePacket(outapp);
	safe_delete(outapp);
}

void Client::SendPreviousAA(uint32 id, int seq){
	uint32 value=0;
	SendAA_Struct* saa2 = nullptr;
	if(id==0)
		saa2 = zone->GetAABySequence(seq);
	else
		saa2 = zone->FindAA(id);
	if(!saa2)
		return;
	int size=sizeof(SendAA_Struct)+sizeof(AA_Ability)*saa2->total_abilities;
	uchar* buffer = new uchar[size];
	SendAA_Struct* saa=(SendAA_Struct*)buffer;
	value = GetAA(saa2->id);
	EQApplicationPacket* outapp = new EQApplicationPacket(OP_SendAATable);
	outapp->size=size;
	outapp->pBuffer=(uchar*)saa;
	value--;
	memcpy(saa,saa2,size);

	if(value>0){
		if(saa->spellid==0)
			saa->spellid=0xFFFFFFFF;
		saa->id+=value;
		saa->next_id=saa->id+1;
		if(value==1)
			saa->last_id=saa2->id;
		else
			saa->last_id=saa->id-1;
		saa->current_level=value+1;
		saa->cost2 = 0; //cost 2 is what the client uses to calc how many points we've spent, so we have to add up the points in order
		for(uint32 i = 0; i < (value+1); i++) {
			saa->cost2 += saa->cost + (saa->cost_inc * i);
		}
	}

	database.FillAAEffects(saa);
	QueuePacket(outapp);
	safe_delete(outapp);
}

void Client::SendAA(uint32 id, int seq) {

	uint32 value=0;
	SendAA_Struct* saa2 = nullptr;
	SendAA_Struct* qaa = nullptr;
	SendAA_Struct* saa_pp = nullptr;
	bool IsBaseLevel = true;
	bool aa_stack = false;

	if(id==0)
		saa2 = zone->GetAABySequence(seq);
	else
		saa2 = zone->FindAA(id);
	if(!saa2)
		return;

	uint16 classes = saa2->classes;
	if(!(classes & (1 << GetClass())) && (GetClass()!=BERSERKER || saa2->berserker==0)){
		return;
	}

	if(saa2->account_time_required)
	{
		if((Timer::GetTimeSeconds() - account_creation) < saa2->account_time_required)
		{
			return;
		}
	}

	// Hide Quest/Progression AAs unless player has been granted the first level using $client->IncrementAA(skill_id).
	if (saa2->special_category == 1 || saa2->special_category == 2 ) {
		if(GetAA(saa2->id) == 0)
			return;
		// For Quest line AA(demiplane AEs) where only 1 is visible at a time, check to make sure only the highest level obtained is shown
		if(saa2->aa_expansion > 0) {
			qaa = zone->FindAA(saa2->id+1);
			if(qaa && (saa2->aa_expansion == qaa->aa_expansion) && GetAA(qaa->id) > 0)
				return;
		}
	}

/*	Beginning of Shroud AAs, these categories are for Passive and Active Shroud AAs
	Eventually with a toggle we could have it show player list or shroud list
	if (saa2->special_category == 3 || saa2->special_category == 4)
		return;
*/
	// Check for racial/Drakkin blood line AAs
	if (saa2->special_category == 8)
	{
		uint32 client_race = this->GetBaseRace();

		// Drakkin Bloodlines
		if (saa2->aa_expansion > 522)
		{
			if (client_race != 522)
				return; // Check for Drakkin Race

			int heritage = this->GetDrakkinHeritage() + 523; // 523 = Drakkin Race(522) + Bloodline

			if (heritage != saa2->aa_expansion)
				return;
		}
		// Racial AAs
		else if (client_race != saa2->aa_expansion)
		{
			return;
		}
	}

	/*
	AA stacking on SoF+ clients.

	Note: There were many ways to achieve this effect - The method used proved to be the most straight forward and consistent.
	Stacking does not currently work ideally for AA's that use hotkeys, therefore they will be excluded at this time.

	TODO: Problem with aa.hotkeys - When you reach max rank of an AA tier (ie 5/5), it automatically displays the next AA in
	the series and you can not transfer the hotkey to the next AA series. To the best of the my ability and through many
	different variations of coding I could not find an ideal solution to this issue.

	How stacking works:
	Utilizes two new fields: sof_next_id (which is the next id in the series), sof_current_level (ranks the AA's as the current level)
	1) If no AA's purchased only display the base levels of each AA series.
	2) When you purchase an AA and its rank is maxed it sends the packet for the completed AA, and the packet
	for the next aa in the series. The previous tier is removed from your window, and the new AA is displayed.
	3) When you zone/buy your player profile will be checked and determine what AA can be displayed base on what you have already.
	*/

	if (RuleB(AA, Stacking) && (GetClientVersionBit() >= 4) && (saa2->hotkey_sid == 4294967295u))
		aa_stack = true;

	if (aa_stack){
		uint32 aa_AA = 0;
		uint32 aa_value = 0;
		for (int i = 0; i < MAX_PP_AA_ARRAY; i++) {
			if (aa[i]) {
				aa_AA = aa[i]->AA;
				aa_value = aa[i]->value;

				if (aa_AA){

					if (aa_value > 0)
						aa_AA -= aa_value-1;

					saa_pp = zone->FindAA(aa_AA);

					if (saa_pp){

						if (saa_pp->sof_next_skill == saa2->sof_next_skill){

							if (saa_pp->id == saa2->id)
								break; //You already have this in the player profile.
							else if ((saa_pp->sof_current_level < saa2->sof_current_level) && (aa_value < saa_pp->max_level))
								return; //DISABLE DISPLAY HIGHER - You have not reached max level yet of your current AA.
							else if ((saa_pp->sof_current_level < saa2->sof_current_level) && (aa_value == saa_pp->max_level) && (saa_pp->sof_next_id == saa2->id))
								IsBaseLevel = false; //ALLOW DISPLAY HIGHER
						}
					}
				}
			}
		}
	}

	//Hide higher tiers of multi tiered AA's if the base level is not fully purchased.
	if (aa_stack && IsBaseLevel && saa2->sof_current_level > 0)
		return;

	int size=sizeof(SendAA_Struct)+sizeof(AA_Ability)*saa2->total_abilities;

	if(size == 0)
		return;

	uchar* buffer = new uchar[size];
	SendAA_Struct* saa=(SendAA_Struct*)buffer;
	memcpy(saa,saa2,size);

	if(saa->spellid==0)
		saa->spellid=0xFFFFFFFF;

	value=GetAA(saa->id);
	uint32 orig_val = value;

	if(value && saa->id){

		if(value < saa->max_level){
			saa->id+=value;
			saa->next_id=saa->id+1;
			value++;
		}

		else if (aa_stack && saa->sof_next_id){
			saa->id+=value-1;
			saa->next_id=saa->sof_next_id;

			//Prevent removal of previous AA from window if next AA belongs to a higher client version.
			SendAA_Struct* saa_next = nullptr;
			saa_next = zone->FindAA(saa->sof_next_id);

			// hard-coding values like this is dangerous and makes adding/updating clients a nightmare...
			if (saa_next &&
				(((GetClientVersionBit() == 4) && (saa_next->clientver > 4))
				|| ((GetClientVersionBit() == 8) && (saa_next->clientver > 5))
				|| ((GetClientVersionBit() == 16) && (saa_next->clientver > 6)))){
				saa->next_id=0xFFFFFFFF;
			}
		}

		else{
			saa->id+=value-1;
			saa->next_id=0xFFFFFFFF;
		}

		uint32 current_level_mod = 0;
		if (aa_stack)
			current_level_mod = saa->sof_current_level;

		saa->last_id=saa->id-1;
		saa->current_level=value+(current_level_mod);
		saa->cost = saa2->cost + (saa2->cost_inc*(value-1));
		saa->cost2 = 0;
		for(uint32 i = 0; i < value; i++) {
			saa->cost2 += saa2->cost + (saa2->cost_inc * i);
		}
		saa->class_type = saa2->class_type + (saa2->level_inc*(value-1));
	}

	if (aa_stack){

		if (saa->sof_current_level >= 1 && value == 0)
			saa->current_level = saa->sof_current_level+1;

		saa->max_level = saa->sof_max_level;
	}

	database.FillAAEffects(saa);

	if(value > 0)
	{
		// AA_Action stores the base ID
		const AA_DBAction *caa = &AA_Actions[saa->id - value + 1][value - 1];

		if(caa && caa->reuse_time > 0)
			saa->spell_refresh = CalcAAReuseTimer(caa);
	}

	//You can now use the level_inc field in the altadv_vars table to accomplish this, though still needed
	//for special cases like LOH/HT due to inability to implement correct stacking of AA's that use hotkeys.
	std::map<uint32, AALevelCost_Struct>::iterator RequiredLevel = AARequiredLevelAndCost.find(saa->id);

	if(RequiredLevel != AARequiredLevelAndCost.end())
	{
		saa->class_type = RequiredLevel->second.Level;
		saa->cost = RequiredLevel->second.Cost;
	}


	EQApplicationPacket* outapp = new EQApplicationPacket(OP_SendAATable);
	outapp->size=size;
	outapp->pBuffer=(uchar*)saa;
	if(id==0 && value && (orig_val < saa->max_level)) //send previous AA only on zone in
		SendPreviousAA(id, seq);

	QueuePacket(outapp);
	safe_delete(outapp);
	//will outapp delete the buffer for us even though it didnt make it? --- Yes, it should
}

void Client::SendAAList(){
	int total = zone->GetTotalAAs();
	for(int i=0;i < total;i++){
		SendAA(0,i);
	}
}

uint32 Client::GetAA(uint32 aa_id) const {
	std::map<uint32,uint8>::const_iterator res;
	res = aa_points.find(aa_id);
	if(res != aa_points.end()) {
		return(res->second);
	}
	return(0);
}

bool Client::SetAA(uint32 aa_id, uint32 new_value) {
	aa_points[aa_id] = new_value;
	uint32 cur;
	for(cur=0;cur < MAX_PP_AA_ARRAY;cur++){
		if((aa[cur]->value > 1) && ((aa[cur]->AA - aa[cur]->value + 1)== aa_id)){
			aa[cur]->value = new_value;
			if(new_value > 0)
				aa[cur]->AA++;
			else
				aa[cur]->AA = 0;
			return true;
		}
		else if((aa[cur]->value == 1) && (aa[cur]->AA == aa_id)){
			aa[cur]->value = new_value;
			if(new_value > 0)
				aa[cur]->AA++;
			else
				aa[cur]->AA = 0;
			return true;
		}
		else if(aa[cur]->AA==0){ //end of list
			aa[cur]->AA = aa_id;
			aa[cur]->value = new_value;
			return true;
		}
	}
	return false;
}

SendAA_Struct* Zone::FindAA(uint32 id) {
	return aas_send[id];
}

void Zone::LoadAAs() {
	Log.Out(Logs::General, Logs::Status, "Loading AA information...");
	totalAAs = database.CountAAs();
	if(totalAAs == 0) {
		Log.Out(Logs::General, Logs::Error, "Failed to load AAs!");
		aas = nullptr;
		return;
	}
	aas = new SendAA_Struct *[totalAAs];

	database.LoadAAs(aas);

	int i;
	for(i=0; i < totalAAs; i++){
		SendAA_Struct* aa = aas[i];
		aas_send[aa->id] = aa;
	}

	//load AA Effects into aa_effects
	Log.Out(Logs::General, Logs::Status, "Loading AA Effects...");
	if (database.LoadAAEffects2())
		Log.Out(Logs::General, Logs::Status, "Loaded %d AA Effects.", aa_effects.size());
	else
		Log.Out(Logs::General, Logs::Error, "Failed to load AA Effects!");
}

bool ZoneDatabase::LoadAAEffects2() {
	aa_effects.clear();	//start fresh

	const std::string query = "SELECT aaid, slot, effectid, base1, base2 FROM aa_effects ORDER BY aaid ASC, slot ASC";
	auto results = QueryDatabase(query);
	if (!results.Success()) {
		return false;
	}

	if (!results.RowCount()) { //no results
		return false;
	}

	for(auto row = results.begin(); row != results.end(); ++row) {
		int aaid = atoi(row[0]);
		int slot = atoi(row[1]);
		int effectid = atoi(row[2]);
		int base1 = atoi(row[3]);
		int base2 = atoi(row[4]);
		aa_effects[aaid][slot].skill_id = effectid;
		aa_effects[aaid][slot].base1 = base1;
		aa_effects[aaid][slot].base2 = base2;
		aa_effects[aaid][slot].slot = slot;	//not really needed, but we'll populate it just in case
	}

	return true;
}

void Client::ResetAA(){
	RefundAA();
	uint32 i;
	for (i=0; i < MAX_PP_AA_ARRAY; i++) {
		aa[i]->AA = 0;
		aa[i]->value = 0;
		m_pp.aa_array[MAX_PP_AA_ARRAY].AA = 0;
		m_pp.aa_array[MAX_PP_AA_ARRAY].value = 0;
	}

	std::map<uint32,uint8>::iterator itr;
	for(itr = aa_points.begin(); itr != aa_points.end(); ++itr)
		aa_points[itr->first] = 0;

	for(int i = 0; i < _maxLeaderAA; ++i)
		m_pp.leader_abilities.ranks[i] = 0;

	m_pp.group_leadership_points = 0;
	m_pp.raid_leadership_points = 0;
	m_pp.group_leadership_exp = 0;
	m_pp.raid_leadership_exp = 0;

	database.DeleteCharacterAAs(this->CharacterID());
	SaveAA();
	SendClearAA();
	SendAAList();
	SendAATable();
	SendAAStats();
	database.DeleteCharacterLeadershipAAs(this->CharacterID());
	// undefined for these clients
	if (GetClientVersionBit() & BIT_TitaniumAndEarlier)
		Kick();
}

void Client::SendClearAA()
{
	EQApplicationPacket *outapp = new EQApplicationPacket(OP_ClearLeadershipAbilities, 0);
	FastQueuePacket(&outapp);
	outapp = new EQApplicationPacket(OP_ClearAA, 0);
	FastQueuePacket(&outapp);
}

int Client::GroupLeadershipAAHealthEnhancement()
{
	if (IsRaidGrouped()) {
		int bonus = 0;
		Raid *raid = GetRaid();
		if (!raid)
			return 0;
		uint32 group_id = raid->GetGroup(this);
		if (group_id < 12 && raid->GroupCount(group_id) >= 3) {
			switch (raid->GetLeadershipAA(groupAAHealthEnhancement, group_id)) {
			case 1:
				bonus = 30;
				break;
			case 2:
				bonus = 60;
				break;
			case 3:
				bonus = 100;
				break;
			}
		}
		if (raid->RaidCount() >= 18) {
			switch (raid->GetLeadershipAA(raidAAHealthEnhancement)) {
			case 1:
				bonus += 30;
				break;
			case 2:
				bonus += 60;
				break;
			case 3:
				bonus += 100;
				break;
			}
		}
		return bonus;
	}

	Group *g = GetGroup();

	if(!g || (g->GroupCount() < 3))
		return 0;

	switch(g->GetLeadershipAA(groupAAHealthEnhancement))
	{
		case 0:
			return 0;
		case 1:
			return 30;
		case 2:
			return 60;
		case 3:
			return 100;
	}

	return 0;
}

int Client::GroupLeadershipAAManaEnhancement()
{
	if (IsRaidGrouped()) {
		int bonus = 0;
		Raid *raid = GetRaid();
		if (!raid)
			return 0;
		uint32 group_id = raid->GetGroup(this);
		if (group_id < 12 && raid->GroupCount(group_id) >= 3) {
			switch (raid->GetLeadershipAA(groupAAManaEnhancement, group_id)) {
			case 1:
				bonus = 30;
				break;
			case 2:
				bonus = 60;
				break;
			case 3:
				bonus = 100;
				break;
			}
		}
		if (raid->RaidCount() >= 18) {
			switch (raid->GetLeadershipAA(raidAAManaEnhancement)) {
			case 1:
				bonus += 30;
				break;
			case 2:
				bonus += 60;
				break;
			case 3:
				bonus += 100;
				break;
			}
		}
		return bonus;
	}

	Group *g = GetGroup();

	if(!g || (g->GroupCount() < 3))
		return 0;

	switch(g->GetLeadershipAA(groupAAManaEnhancement))
	{
		case 0:
			return 0;
		case 1:
			return 30;
		case 2:
			return 60;
		case 3:
			return 100;
	}

	return 0;
}

int Client::GroupLeadershipAAHealthRegeneration()
{
	if (IsRaidGrouped()) {
		int bonus = 0;
		Raid *raid = GetRaid();
		if (!raid)
			return 0;
		uint32 group_id = raid->GetGroup(this);
		if (group_id < 12 && raid->GroupCount(group_id) >= 3) {
			switch (raid->GetLeadershipAA(groupAAHealthRegeneration, group_id)) {
			case 1:
				bonus = 4;
				break;
			case 2:
				bonus = 6;
				break;
			case 3:
				bonus = 8;
				break;
			}
		}
		if (raid->RaidCount() >= 18) {
			switch (raid->GetLeadershipAA(raidAAHealthRegeneration)) {
			case 1:
				bonus += 4;
				break;
			case 2:
				bonus += 6;
				break;
			case 3:
				bonus += 8;
				break;
			}
		}
		return bonus;
	}

	Group *g = GetGroup();

	if(!g || (g->GroupCount() < 3))
		return 0;

	switch(g->GetLeadershipAA(groupAAHealthRegeneration))
	{
		case 0:
			return 0;
		case 1:
			return 4;
		case 2:
			return 6;
		case 3:
			return 8;
	}

	return 0;
}

int Client::GroupLeadershipAAOffenseEnhancement()
{
	if (IsRaidGrouped()) {
		int bonus = 0;
		Raid *raid = GetRaid();
		if (!raid)
			return 0;
		uint32 group_id = raid->GetGroup(this);
		if (group_id < 12 && raid->GroupCount(group_id) >= 3) {
			switch (raid->GetLeadershipAA(groupAAOffenseEnhancement, group_id)) {
			case 1:
				bonus = 10;
				break;
			case 2:
				bonus = 19;
				break;
			case 3:
				bonus = 28;
				break;
			case 4:
				bonus = 34;
				break;
			case 5:
				bonus = 40;
				break;
			}
		}
		if (raid->RaidCount() >= 18) {
			switch (raid->GetLeadershipAA(raidAAOffenseEnhancement)) {
			case 1:
				bonus += 10;
				break;
			case 2:
				bonus += 19;
				break;
			case 3:
				bonus += 28;
				break;
			case 4:
				bonus += 34;
				break;
			case 5:
				bonus += 40;
				break;
			}
		}
		return bonus;
	}

	Group *g = GetGroup();

	if(!g || (g->GroupCount() < 3))
		return 0;

	switch(g->GetLeadershipAA(groupAAOffenseEnhancement))
	{
		case 0:
			return 0;
		case 1:
			return 10;
		case 2:
			return 19;
		case 3:
			return 28;
		case 4:
			return 34;
		case 5:
			return 40;
	}
	return 0;
}

void Client::InspectBuffs(Client* Inspector, int Rank)
{
	// At some point the removed the restriction of being a group member for this to work
	// not sure when, but the way it's coded now, it wouldn't work with mobs.
	if (!Inspector || Rank == 0)
		return;

	EQApplicationPacket *outapp = new EQApplicationPacket(OP_InspectBuffs, sizeof(InspectBuffs_Struct));
	InspectBuffs_Struct *ib = (InspectBuffs_Struct *)outapp->pBuffer;

	uint32 buff_count = GetMaxTotalSlots();
	uint32 packet_index = 0;
	for (uint32 i = 0; i < buff_count; i++) {
		if (buffs[i].spellid == SPELL_UNKNOWN)
			continue;
		ib->spell_id[packet_index] = buffs[i].spellid;
		if (Rank > 1)
			ib->tics_remaining[packet_index] = spells[buffs[i].spellid].buffdurationformula == DF_Permanent ? 0xFFFFFFFF : buffs[i].ticsremaining;
		packet_index++;
	}

	Inspector->FastQueuePacket(&outapp);
}

//this really need to be renamed to LoadAAActions()
bool ZoneDatabase::LoadAAEffects() {
	memset(AA_Actions, 0, sizeof(AA_Actions));	//I hope the compiler is smart about this size...

	const std::string query = "SELECT aaid, rank, reuse_time, spell_id, target, "
							"nonspell_action, nonspell_mana, nonspell_duration, "
							"redux_aa, redux_rate, redux_aa2, redux_rate2 FROM aa_actions";
	auto results = QueryDatabase(query);
	if (!results.Success()) {
		return false;
	}

	for (auto row = results.begin(); row != results.end(); ++row) {

		int aaid = atoi(row[0]);
		int rank = atoi(row[1]);
		if(aaid < 0 || aaid >= aaHighestID || rank < 0 || rank >= MAX_AA_ACTION_RANKS)
			continue;
		AA_DBAction *caction = &AA_Actions[aaid][rank];

		caction->reuse_time = atoi(row[2]);
		caction->spell_id = atoi(row[3]);
		caction->target = (aaTargetType) atoi(row[4]);
		caction->action = (aaNonspellAction) atoi(row[5]);
		caction->mana_cost = atoi(row[6]);
		caction->duration = atoi(row[7]);
		caction->redux_aa = (aaID) atoi(row[8]);
		caction->redux_rate = atoi(row[9]);
		caction->redux_aa2 = (aaID) atoi(row[10]);
		caction->redux_rate2 = atoi(row[11]);

	}

	return true;
}

//Returns the number effects an aa.has when we send them to the client
//For the purposes of sizing a packet because every skill does not
//have the same number effects, they can range from none to a few depending on AA.
//counts the # of effects by counting the different slots of an AAID in the DB.

//AndMetal: this may now be obsolete since we have Zone::GetTotalAALevels()
uint8 ZoneDatabase::GetTotalAALevels(uint32 skill_id) {

	std::string query = StringFormat("SELECT count(slot) FROM aa_effects WHERE aaid = %i", skill_id);
    auto results = QueryDatabase(query);
    if (!results.Success()) {
        return 0;
    }

	if (results.RowCount() != 1)
		return 0;

	auto row = results.begin();

	return atoi(row[0]);
}

//this will allow us to count the number of effects for an AA by pulling the info from memory instead of the database. hopefully this will same some CPU cycles
uint8 Zone::GetTotalAALevels(uint32 skill_id) {
	size_t sz = aa_effects[skill_id].size();
	return sz >= 255 ? 255 : static_cast<uint8>(sz);
}

/*
Every AA can send the client effects, which are purely for client side effects.
Essentially it's like being able to attach a very simple version of a spell to
Any given AA, it has 4 fields:
skill_id = spell effect id
slot = ID slot, doesn't appear to have any impact on stacking like real spells, just needs to be unique.
base1 = the base field of a spell
base2 = base field 2 of a spell, most AAs do not utilize this
example:
	skill_id = SE_STA
	slot = 1
	base1 = 15
	This would if you filled the abilities struct with this make the client show if it had
	that AA an additional 15 stamina on the client's stats
*/
void ZoneDatabase::FillAAEffects(SendAA_Struct* aa_struct){
	if(!aa_struct)
		return;

	auto it = aa_effects.find(aa_struct->id);
	if (it != aa_effects.end()) {
		for (uint32 slot = 0; slot < aa_struct->total_abilities; slot++) {
			// aa_effects is a map of a map, so the slot reference does not start at 0
			aa_struct->abilities[slot].skill_id = it->second[slot + 1].skill_id;
			aa_struct->abilities[slot].base1 = it->second[slot + 1].base1;
			aa_struct->abilities[slot].base2 = it->second[slot + 1].base2;
			aa_struct->abilities[slot].slot = it->second[slot + 1].slot;
		}
	}
}

uint32 ZoneDatabase::CountAAs(){

	const std::string query = "SELECT count(title_sid) FROM altadv_vars";
	auto results = QueryDatabase(query);
	if (!results.Success()) {
        return 0;
	}

	if (results.RowCount() != 1)
		return 0;

	auto row = results.begin();

	return atoi(row[0]);;
}

uint32 ZoneDatabase::CountAAEffects() {

	const std::string query = "SELECT count(id) FROM aa_effects";
	auto results = QueryDatabase(query);
	if (!results.Success()) {
        return 0;
	}

	if (results.RowCount() != 1)
		return 0;

	auto row = results.begin();

	return atoi(row[0]);
}

uint32 ZoneDatabase::GetSizeAA(){
	int size=CountAAs()*sizeof(SendAA_Struct);
	if(size>0)
		size+=CountAAEffects()*sizeof(AA_Ability);
	return size;
}

void ZoneDatabase::LoadAAs(SendAA_Struct **load){
	if(!load)
		return;

	std::string query = "SELECT skill_id FROM altadv_vars ORDER BY skill_id";
	auto results = QueryDatabase(query);
	if (results.Success()) {
		int skill = 0, index = 0;
		for (auto row = results.begin(); row != results.end(); ++row, ++index) {
			skill = atoi(row[0]);
			load[index] = GetAASkillVars(skill);
			load[index]->seq = index+1;
		}
	} else {
	}

	AARequiredLevelAndCost.clear();
	query = "SELECT skill_id, level, cost from aa_required_level_cost order by skill_id";
	results = QueryDatabase(query);
	if (!results.Success()) {
		return;
	}

	AALevelCost_Struct aalcs;
	for (auto row = results.begin(); row != results.end(); ++row) {
		aalcs.Level = atoi(row[1]);
		aalcs.Cost = atoi(row[2]);
		AARequiredLevelAndCost[atoi(row[0])] = aalcs;
	}
}

SendAA_Struct* ZoneDatabase::GetAASkillVars(uint32 skill_id)
{
	std::string query = "SET @row = 0"; //initialize "row" variable in database for next query
	auto results = QueryDatabase(query);
	if (!results.Success()) {
		return nullptr;
	}

	query = StringFormat("SELECT a.cost, a.max_level, a.hotkey_sid, a.hotkey_sid2, a.title_sid, a.desc_sid, a.type, "
						"COALESCE("	//So we can return 0 if it's null.
						"("	// this is our derived table that has the row #
							// that we can SELECT from, because the client is stupid.
						"SELECT p.prereq_index_num "
						"FROM (SELECT a2.skill_id, @row := @row + 1 AS prereq_index_num "
						"FROM altadv_vars a2) AS p "
						"WHERE p.skill_id = a.prereq_skill), 0) "
						"AS prereq_skill_index, a.prereq_minpoints, a.spell_type, a.spell_refresh, a.classes, "
						"a.berserker, a.spellid, a.class_type, a.name, a.cost_inc, a.aa_expansion, a.special_category, "
						"a.sof_type, a.sof_cost_inc, a.sof_max_level, a.sof_next_skill, "
						"a.clientver, "	// Client Version 0 = None, 1 = All, 2 = Titanium/6.2, 4 = SoF 5 = SOD 6 = UF
						"a.account_time_required, a.sof_current_level, a.sof_next_id, a.level_inc "
						"FROM altadv_vars a WHERE skill_id=%i", skill_id);
	results = QueryDatabase(query);
	if (!results.Success()) {
		return nullptr;
	}

	if (results.RowCount() != 1)
		return nullptr;

	int total_abilities = GetTotalAALevels(skill_id);	//eventually we'll want to use zone->GetTotalAALevels(skill_id) since it should save queries to the DB
	int totalsize = total_abilities * sizeof(AA_Ability) + sizeof(SendAA_Struct);

	SendAA_Struct* sendaa = nullptr;
	uchar* buffer;

	buffer = new uchar[totalsize];
	memset(buffer,0,totalsize);
	sendaa = (SendAA_Struct*)buffer;

	auto row = results.begin();

	//ATOI IS NOT UNSIGNED LONG-SAFE!!!

	sendaa->cost = atoul(row[0]);
	sendaa->cost2 = sendaa->cost;
	sendaa->max_level = atoul(row[1]);
	sendaa->hotkey_sid = atoul(row[2]);
	sendaa->id = skill_id;
	sendaa->hotkey_sid2 = atoul(row[3]);
	sendaa->title_sid = atoul(row[4]);
	sendaa->desc_sid = atoul(row[5]);
	sendaa->type = atoul(row[6]);
	sendaa->prereq_skill = atoul(row[7]);
	sendaa->prereq_minpoints = atoul(row[8]);
	sendaa->spell_type = atoul(row[9]);
	sendaa->spell_refresh = atoul(row[10]);
	sendaa->classes = static_cast<uint16>(atoul(row[11]));
	sendaa->berserker = static_cast<uint16>(atoul(row[12]));
	sendaa->last_id = 0xFFFFFFFF;
	sendaa->current_level=1;
	sendaa->spellid = atoul(row[13]);
	sendaa->class_type = atoul(row[14]);
	strcpy(sendaa->name,row[15]);

	sendaa->total_abilities=total_abilities;
	if(sendaa->max_level > 1)
		sendaa->next_id=skill_id+1;
	else
		sendaa->next_id=0xFFFFFFFF;

	sendaa->cost_inc = atoi(row[16]);

	// Begin SoF Specific/Adjusted AA Fields
	sendaa->aa_expansion = atoul(row[17]);
	sendaa->special_category = atoul(row[18]);
	sendaa->sof_type = atoul(row[19]);
	sendaa->sof_cost_inc = atoi(row[20]);
	sendaa->sof_max_level = atoul(row[21]);
	sendaa->sof_next_skill = atoul(row[22]);
	sendaa->clientver = atoul(row[23]);
	sendaa->account_time_required = atoul(row[24]);

	//Internal use only - not sent to client
	sendaa->sof_current_level = atoul(row[25]);
	sendaa->sof_next_id = atoul(row[26]);
	sendaa->level_inc = static_cast<uint8>(atoul(row[27]));

	return sendaa;
}

void Client::DurationRampage(uint32 duration)
{
	if(duration) {
		m_epp.aa_effects |= 1 << (aaEffectRampage-1);
		p_timers.Start(pTimerAAEffectStart + aaEffectRampage, duration);
	}
}

AA_SwarmPetInfo::AA_SwarmPetInfo()
{
	target = 0;
	owner_id = 0;
	duration = nullptr;
}

AA_SwarmPetInfo::~AA_SwarmPetInfo()
{
	target = 0;
	owner_id = 0;
	safe_delete(duration);
}

Mob *AA_SwarmPetInfo::GetOwner()
{
	return entity_list.GetMobID(owner_id);
}
