/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "cache.h"
#include "hash.h"

#include "event_recorder.h"
#include "timing_event.h"
#include "zsim.h"

Cache::Cache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const g_string& _name)
    : cc(_cc), array(_array), rp(_rp), numLines(_numLines), accLat(_accLat), invLat(_invLat), name(_name) {}

const char* Cache::getName() {
    return name.c_str();
}

void Cache::setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network) {
    cc->setParents(childId, parents, network);
}

void Cache::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    cc->setChildren(children, network);
}

void Cache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Cache stats");
    initCacheStats(cacheStat);
    parentStat->append(cacheStat);
}

void Cache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    array->initStats(cacheStat);
    rp->initStats(cacheStat);
}

uint64_t Cache::access(MemReq& req) {
    uint64_t respCycle = req.cycle;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t lineId = array->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;

        if (lineId == -1 && cc->shouldAllocate(req)) {
            //Make space for new line
            Address wbLineAddr;
            lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

            //Evictions are not in the critical path in any sane implementation -- we do not include their delays
            //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
            cc->processEviction(req, wbLineAddr, lineId, respCycle); //1. if needed, send invalidates/downgrades to lower level

            array->postinsert(req.lineAddr, &req, lineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.
        }
        // Enforce single-record invariant: Writeback access may have a timing
        // record. If so, read it.
        EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
        TimingRecord wbAcc;
        wbAcc.clear();
        if (unlikely(evRec && evRec->hasRecord())) {
            wbAcc = evRec->popRecord();
        }

        respCycle = cc->processAccess(req, lineId, respCycle);

        // Access may have generated another timing record. If *both* access
        // and wb have records, stitch them together
        if (unlikely(wbAcc.isValid())) {
            if (!evRec->hasRecord()) {
                // Downstream should not care about endEvent for PUTs
                wbAcc.endEvent = nullptr;
                evRec->pushRecord(wbAcc);
            } else {
                // Connect both events
                TimingRecord acc = evRec->popRecord();
                assert(wbAcc.reqCycle >= req.cycle);
                assert(acc.reqCycle >= req.cycle);
                DelayEvent* startEv = new (evRec) DelayEvent(0);
                DelayEvent* dWbEv = new (evRec) DelayEvent(wbAcc.reqCycle - req.cycle);
                DelayEvent* dAccEv = new (evRec) DelayEvent(acc.reqCycle - req.cycle);
                startEv->setMinStartCycle(req.cycle);
                dWbEv->setMinStartCycle(req.cycle);
                dAccEv->setMinStartCycle(req.cycle);
                startEv->addChild(dWbEv, evRec)->addChild(wbAcc.startEvent, evRec);
                startEv->addChild(dAccEv, evRec)->addChild(acc.startEvent, evRec);

                acc.reqCycle = req.cycle;
                acc.startEvent = startEv;
                // endEvent / endCycle stay the same; wbAcc's endEvent not connected
                evRec->pushRecord(acc);
            }
        }
    }

    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

// JIN
static bool findSubstring(const char* string, const char* substring)
{
	unsigned i = 0;
	unsigned j = 0;
	while(string[i] != '\0')
	{
		if(string[i] == substring[j])
		{
			j++;
			if(substring[j] == '\0')
				return true;
		}
		else
		{
			j = 0;
		}

		i++;
	}
	return false;
}

// JIN
static uint8_t enumAccessType(AccessType t)
{
	if(t == GETS)				return 0;
	else if(t == GETX)	return 1;
	else if(t == PUTS)	return 2;
	else if(t == PUTX)	return 3;
	else {
		printf("enum error!\n");
		exit(1);
	}
}

// JIN
// Assume that DRAM is inclusive of the LLC.
// Because non-inclusive LLC is not implemented in the ZSim as far as I know.
void Cache::PrintData(MemReq& req, bool isMiss)
{
	if(zinfo->data_trace_output_FP != NULL)
		return;

	if(!findSubstring(name.c_str(), zinfo->llcName))
		return;

	DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
	Address reqAddress = req.lineAddr << lineBits;
	PIN_SafeCopy(data, (void*)reqAddress, zinfo->lineSize);

	futex_lock(&zinfo->printLock);
	// Read
	if(req.type == GETS || req.type == GETX)
	{
		if(isMiss)
		{
			printf("DRAM_R, %s, %lu, %s, 0x%016lx, ", name.c_str(), req.cycle, AccessTypeName(req.type), reqAddress);
	
			for(unsigned i = 0; i < zinfo->lineSize; i++)
				printf("%02x,", ((uint8_t*)data)[i]);
			printf("\n");
		}
	}
	// Write-through (Non-inclusive)
	else
	{
		printf("DRAM_W, %s, %lu, %s, 0x%016lx, ", name.c_str(), req.cycle, AccessTypeName(req.type), reqAddress);

		for(unsigned i = 0; i < zinfo->lineSize; i++)
			printf("%02x,", ((uint8_t*)data)[i]);
		printf("\n");
	}
	futex_unlock(&zinfo->printLock);

	gm_free(data);
}

// JIN
void Cache::WriteData(MemReq& req, bool isMiss)
{
	if(zinfo->data_trace_output_FP == NULL)
		return;

	if(!findSubstring(name.c_str(), zinfo->llcName))
		return;

	DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
	Address reqAddress = req.lineAddr << lineBits;
	PIN_SafeCopy(data, (void*)reqAddress, zinfo->lineSize);

	futex_lock(&zinfo->printLock);
	// Read
	if(req.type == GETS || req.type == GETX)
	{
		if(isMiss)
		{
			char rw = 'r';
			fwrite(&rw, sizeof(char), 1, zinfo->data_trace_output_FP);

			const char* o_name = name.c_str();
			fwrite(o_name, sizeof(char), 10, zinfo->data_trace_output_FP);

			uint64_t cycle = req.cycle;
			fwrite(&cycle, sizeof(uint64_t), 1, zinfo->data_trace_output_FP);

			uint8_t type = enumAccessType(req.type);
			fwrite(&type, sizeof(uint8_t), 1, zinfo->data_trace_output_FP);

			uint64_t addr = reqAddress;
			fwrite(&addr, sizeof(uint64_t), 1, zinfo->data_trace_output_FP);

			fwrite(data, sizeof(uint8_t), zinfo->lineSize, zinfo->data_trace_output_FP);
		}
	}
	// Write-through (Non-inclusive)
	else
	{
			char rw = 'w';
			fwrite(&rw, sizeof(char), 1, zinfo->data_trace_output_FP);

			const char* o_name = name.c_str();
			fwrite(o_name, sizeof(char), 10, zinfo->data_trace_output_FP);

			uint64_t cycle = req.cycle;
			fwrite(&cycle, sizeof(uint64_t), 1, zinfo->data_trace_output_FP);

			uint8_t type = enumAccessType(req.type);
			fwrite(&type, sizeof(uint8_t), 1, zinfo->data_trace_output_FP);

			uint64_t addr = reqAddress;
			fwrite(&addr, sizeof(uint64_t), 1, zinfo->data_trace_output_FP);

			fwrite(data, sizeof(uint8_t), zinfo->lineSize, zinfo->data_trace_output_FP);
	}
	futex_unlock(&zinfo->printLock);

	gm_free(data);
}

void Cache::startInvalidate() {
    cc->startInv(); //note we don't grab tcc; tcc serializes multiple up accesses, down accesses don't see it
}

uint64_t Cache::finishInvalidate(const InvReq& req) {
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false);
    assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    uint64_t respCycle = req.cycle + invLat;
    trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    respCycle = cc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
    trace(Cache, "[%s] Invalidate end 0x%lx type %s lineId %d, reqWriteback %d, latency %ld", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback, respCycle - req.cycle);

    return respCycle;
}
