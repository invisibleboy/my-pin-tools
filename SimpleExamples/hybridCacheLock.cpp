/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2011 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
//
// @ORIGINAL_AUTHOR: Artur Klauser
// @EXTENDED: Rodric Rabbah (rodric@gmail.com) 
//

/*! @file
 *  This file contains an ISA-portable cache simulator
 *  data cache hierarchies
 */


#include "pin.H"

#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include "hybridCacheLock.h"

#undef USER_PROFILE
//#define USER_PROFILE



/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE,    "pintool",
    "o", "hybrid.out", "specify dcache file name");
KNOB<UINT32> KnobCacheSize(KNOB_MODE_WRITEONCE, "pintool",
    "c","32", "cache size in kilobytes");
KNOB<UINT32> KnobLineSize(KNOB_MODE_WRITEONCE, "pintool",
    "b","32", "cache block size in bytes");
KNOB<UINT32> KnobAssociativity(KNOB_MODE_WRITEONCE, "pintool",
    "a","4", "cache associativity (1 for direct mapped)");
KNOB<string> KnobLockFile(KNOB_MODE_WRITEONCE, "pintool",
	"lf", "lockfile", "lock file");
KNOB<UINT32> KnobTopN(KNOB_MODE_WRITEONCE, "pintool",
    "n","25", "top n value");

/* ===================================================================== */
/* Global variables */
/* ===================================================================== */
UINT32 g_nHeapBegin = 0;
ADDRINT g_nCurFunc = 0;
map<string, set<int> > g_UserfuncSet;
map<ADDRINT, set<int> > g_hFunc2Block;   // user functions, blocks to lock per function
map<ADDRINT, string> g_hAddr2Func;	// user function, start address to function
//map<ADDRINT, string> g_hFunc2Addr;
map<ADDRINT, ADDRINT> g_hAddr2Ebp;		// user function's ebp
#ifdef USER_PROFILE
bool g_bStartSimu = false;	
#endif
/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */

INT32 Usage()
{
    cerr <<
        "This tool represents a cache simulator.\n"
        "\n";

    cerr << KNOB_BASE::StringKnobSummary() << endl; 
    return -1;
}

namespace DL1
{
    const UINT32 max_sets = 16 * KILO; // cacheSize / (lineSize * associativity);
    const UINT32 max_associativity = 16; // associativity;
    const CACHE_ALLOC::STORE_ALLOCATION allocation = CACHE_ALLOC::STORE_ALLOCATE;
	
	typedef HYBRID_CACHE<CACHE_SET::HYBRID_LRU_SET<max_associativity>, max_sets, allocation> CACHE;
}

DL1::CACHE* dl1 = NULL;

/* ===================================================================== */
/* Helper functions                                                    */
/* ===================================================================== */
#ifdef USER_PROFILE
void SetSimu()
{
	g_bStartSimu = true;
}
void UnsetSimu()
{
	g_bStartSimu = false;
}
#endif
// get user function

void GetUserFunc()
{
	std::ifstream inf;
	double topN = (double)KnobTopN.Value();
	inf.open(KnobLockFile.Value().c_str());
	std::string szLine;
	std::string szFunc;
	std::stringstream ss;
	char cLine[256];	
	
	while(inf.getline(cLine, 255) )
	{
		szLine = cLine;
		if( szLine.size() < 4 )
			continue;
		if( szLine.find("##") == 0 )
		{
			szFunc = szLine.substr(2);
		}
		else 
		{	
			int nBlockID;
			double dFreq;
			
			ss.clear();
			ss << szLine;
			ss >> nBlockID;
			ss >> dFreq;
			
			if( dFreq < topN )
				continue;
			g_UserfuncSet[szFunc].insert(nBlockID);
#ifdef QALI_DEBUG
			cerr << "--" << szFunc << ": " << nBlockID << endl;
#endif
		}
	}
	inf.close();
}

/* ===================================================================== */

/*======================================================================*/
VOID LoadMulti(ADDRINT addr, UINT32 size)
{
    // first level D-cache
#ifdef USER_PROFILE
	if( g_bStartSimu)
#endif
	dl1->Access(addr, size, ACCESS_BASE::ACCESS_TYPE_LOAD, true, true );
}

/* ===================================================================== */

VOID StoreMulti(ADDRINT addr, UINT32 size)
{
#ifdef USER_PROFILE
	if( g_bStartSimu)
#endif
    dl1->Access(addr, size, ACCESS_BASE::ACCESS_TYPE_STORE, true, true );
}

/* ===================================================================== */

VOID LoadSingle(ADDRINT addr)
{
#ifdef USER_PROFILE
	if( g_bStartSimu)
#endif
    dl1->AccessSingleLine(addr, ACCESS_BASE::ACCESS_TYPE_LOAD, true, true );
}
/* ===================================================================== */

VOID StoreSingle(ADDRINT addr)
{
#ifdef USER_PROFILE
	if( g_bStartSimu)
#endif
    dl1->AccessSingleLine(addr, ACCESS_BASE::ACCESS_TYPE_STORE, true, true );
}


VOID SetEbp(ADDRINT nAddr, CONTEXT *ctxt)
{
	ADDRINT nEsp = PIN_GetContextReg(ctxt, REG_STACK_PTR);
	g_hAddr2Ebp[nAddr] = nEsp + 4;
#ifdef QALI_DEBUG
	cerr << "==Ebp for " << hex << nAddr << "[" << g_hAddr2Func[nAddr] << "]:\t" << nEsp+4 << endl;
#endif
}
/* ===================================================================== */
// get start address of each user function
VOID Image(IMG img, void *v)
{
/*	map<string, set<int> >::iterator s2s_p = g_UserfuncSet.begin(), s2s_e = g_UserfuncSet.end();
	for(; s2s_p != s2s_e; ++ s2s_p )
	{
		RTN rtn = RTN_FindByName(img, s2s_p->first.c_str());
		ADDRINT nAddr = RTN_Address(rtn);
		g_hFunc2Block[nAddr].insert(s2s_p->second.begin(), s2s_p->second.end());
		g_hAddr2Func[nAddr] = s2s_p->first;
#ifdef QALI_DEBUG
		cerr << "++++" << s2s_p->first << ":\t" <<hex << nAddr << dec << endl;
#endif*/
		//g_hFunc2Addr[s2s_p->first] = nAddr;
	//}	
}


VOID Routine(RTN rtn, void *v)
{
#ifdef USER_PROFILE
	// control the simulation 
/*	string szFunc = RTN_Name(rtn);
	if(szFunc == "main")
	{
		RTN_Open(rtn);
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)SetSimu, IARG_END);
		RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)UnsetSimu, IARG_END);
		RTN_Close(rtn);	
	}*/
#endif	
	
	const string &szName = RTN_Name(rtn); 
	map<string, set<int> >::iterator s2s_p = g_UserfuncSet.find(szName);
	if( s2s_p != g_UserfuncSet.end())	{
		
		// get name to address
		ADDRINT nAddr = RTN_Address(rtn);
		g_hFunc2Block[nAddr].insert(s2s_p->second.begin(), s2s_p->second.end());
		g_hAddr2Func[nAddr] = s2s_p->first;
#ifdef QALI_DEBUG
		cerr << "++++" << s2s_p->first << ":\t" <<hex << nAddr << dec << endl;
#endif

		// add instrumentation code for ebp
		RTN_Open(rtn);
		RTN_InsertCall( 
			rtn, IPOINT_BEFORE, (AFUNPTR)SetEbp,
			IARG_ADDRINT, nAddr,
			IARG_CONTEXT, IARG_END);
		RTN_Close(rtn);
	}		
}

/* ===================================================================== */
VOID Instruction(INS ins, void * v)
{			
			
    if (INS_IsMemoryRead(ins))
    {
        // map sparse INS addresses to dense IDs
        const UINT32 size = INS_MemoryReadSize(ins);
        const BOOL   single = (size <= 4);
		RTN rtn = INS_Rtn(ins);
		g_nCurFunc = RTN_Address(rtn);		
                
		if( single )
		{
			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE, (AFUNPTR) LoadSingle,
				IARG_MEMORYREAD_EA,
				IARG_END);
		}
		else
		{
			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE,  (AFUNPTR) LoadMulti,
				IARG_MEMORYREAD_EA,
				IARG_MEMORYREAD_SIZE,
				IARG_END);
		}
    }
        
    if ( INS_IsMemoryWrite(ins) )
    {
        // map sparse INS addresses to dense IDs            
        const UINT32 size = INS_MemoryWriteSize(ins);
        const BOOL   single = (size <= 4);
		RTN rtn = INS_Rtn(ins);
		g_nCurFunc = RTN_Address(rtn);
		if( single )
		{
			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE,  (AFUNPTR) StoreSingle,
				IARG_MEMORYWRITE_EA,
				IARG_END);
		}
		else
		{
			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE,  (AFUNPTR) StoreMulti,
				IARG_MEMORYWRITE_EA,
				IARG_MEMORYWRITE_SIZE,
				IARG_END);
		}
    }
}

/* ===================================================================== */

VOID Fini(int code, VOID * v)
{

    std::ofstream out(KnobOutputFile.Value().c_str());

    // print D-cache profile
    // @todo what does this print
    
    out << "PIN:MEMLATENCIES 1.0. 0x0\n";
            
    out <<
        "#\n"
        "# DCACHE stats\n"
        "#\n";    
    
	out << dl1->Dump("#", CACHE_BASE::CACHE_TYPE_DCACHE);
	
      
    out.close();
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    PIN_InitSymbols();

    if( PIN_Init(argc,argv) )
    {
        return Usage();
    }

    dl1 = new DL1::CACHE("L1 Data Cache", 
                         KnobCacheSize.Value() * KILO,
                         KnobLineSize.Value(),
                         KnobAssociativity.Value());    
    GetUserFunc();
	//IMG_AddInstrumentFunction( Image, 0);
	RTN_AddInstrumentFunction(Routine, 0);
    INS_AddInstrumentFunction(Instruction, 0);
	

	
    PIN_AddFiniFunction(Fini, 0);

    // Never returns

    PIN_StartProgram();
	
	delete dl1;
    
    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */