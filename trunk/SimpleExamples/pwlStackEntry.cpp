/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2013 Intel Corporation. All rights reserved.
 
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
#include <set>
#include <list>
#include <map>

//#include "dcache.H"
#include "pin_profile.H"

//#define ONLY_MAIN


/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE,    "pintool",
    "o", "stack.out", "specify dcache file name");
KNOB<UINT32> KnobProfDistance(KNOB_MODE_WRITEONCE, "pintool",
    "d","3", "profing distance power: n -> 2^n");



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

/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */

//typedef int64_t INT64;

UINT32 g_nProfDistPower = 2;
std::ofstream g_outFile;


UINT64 g_nFuncCount = 0;		  // unique ID for each function instance
std::list<UINT64> g_InstanceStack;   // the function instance stack
std::list<ADDRINT> g_RetStack; // stack for return addresses

std::list<pair<UINT64, bool> > g_InstanceTrace; 

// storage for efficiency
std::map<ADDRINT, string> g_hAddr2Func;



std::map<UINT64, UINT64> g_hFuncInst2W; // map function instance to number of writes
std::map<UINT64, UINT64> g_hFuncInst2Addr; // map function instance to function address
std::map<ADDRINT, UINT32> g_hFunc2StackSize; // map function address to stack size

// switch to control the profiling
bool g_bEnable = false;



/* ===================================================================== */
VOID DumpSpace(UINT32 num)
{
	for(UINT32 i = 0; i< num; ++ i)
		cerr << ' ';
}


VOID BeforeCall(ADDRINT nextAddr, ADDRINT callee )
{
#ifdef ONLY_MAIN	
	string szFunc = g_hAddr2Func[callee];
	if( szFunc == "main")
		g_bEnable = true;

	if( !g_bEnable)
		return;
#endif
	
	//DumpSpace(g_instanceStack.size());	
	
	++ g_nFuncCount;	
	g_hFuncInst2Addr[g_nFuncCount] = callee;
	g_InstanceStack.push_back(g_nFuncCount);			
	g_InstanceTrace.push_back(pair<UINT64, bool>(g_nFuncCount, true) );

	g_RetStack.push_back(nextAddr);
}

VOID AfterCall(ADDRINT iAddr )
{
	if( g_RetStack.empty() )
		return;

	//DumpSpace(g_instanceStack.size());	
	
	
	if( iAddr == g_RetStack.back() )  // to identify a function return address
	{
		g_InstanceTrace.push_back(pair<UINT64, bool>(g_InstanceStack.back(), false) );

#ifdef ONLY_MAIN	
		ADDRINT fAddr = g_hFuncInst2Addr[g_InstanceStack.back()];
		string szFunc = g_hAddr2Func[fAddr];
		if( szFunc == "main")
			g_bEnable = false;

		if( !g_bEnable)
			return;
#endif
		g_InstanceStack.pop_back();
		g_RetStack.pop_back();
	}
}


/* ===================================================================== */

VOID StoreMulti(ADDRINT addr, UINT64 size)
{
#ifdef ONLY_MAIN
	if(!g_bEnable)
		return;
#endif
	if( addr < 0x10000000 )
		return;
    	UINT64 nSize = size >> 2;     // 32-bit memory width, which equals 2^2 bytes
	//UINT64 alignedAddr = addr >> g_nProfDistPower;
	//g_WriteR[alignedAddr] += nSize;
	UINT64 nFuncI = g_InstanceStack.back();
	g_hFuncInst2W[nFuncI] += nSize;
}

/* ===================================================================== */

VOID StoreSingle(ADDRINT addr, ADDRINT iaddr)
{
#ifdef ONLY_MAIN
	if(!g_bEnable)
		return;
#endif
	if( addr < 0x10000000 )
		return;
	//UINT64 alignedAddr = addr >> g_nProfDistPower;
    	//++ g_WriteR[alignedAddr];
	UINT64 nFuncI = g_InstanceStack.back();
	++ g_hFuncInst2W[nFuncI];
	
}

/* ===================================================================== */
VOID Routine(RTN rtn, void *v)
{
	RTN_Open(rtn);

	// collect user functions as well as their addresses
	ADDRINT fAddr = RTN_Address(rtn);	
	string szFunc = RTN_Name(rtn);
	g_hAddr2Func[fAddr] = szFunc;
	//cerr << fAddr << ":\t" << szFunc << endl;
	
	for(INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins) )
	{
		// collecting stack size according to "SUB 0x20, %esp" or "ADD 0xffffffe0, %esp"
		if( INS_Opcode(ins) == XED_ICLASS_SUB && 
			INS_OperandIsReg(ins, 0 ) && INS_OperandReg(ins, 0) == REG_STACK_PTR  &&
			INS_OperandIsImmediate(ins, 1) )
		{		                         
           	int nOffset = INS_OperandImmediate(ins, 1);
			if(nOffset < 0 )
			{
				nOffset = -nOffset;
			}           	
			g_hFunc2StackSize[fAddr] = nOffset;	                        
		}
		else if( INS_Opcode(ins) == XED_ICLASS_ADD && 
			INS_OperandIsReg(ins, 0 ) && INS_OperandReg(ins, 0) == REG_STACK_PTR  &&
			INS_OperandIsImmediate(ins, 1) )
		{
			int nOffset = INS_OperandImmediate(ins, 1);
			if(nOffset < 0 )
			{
				nOffset = -nOffset;
			}           	
			g_hFunc2StackSize[fAddr] = nOffset;	  
		}		
	}
	RTN_Close(rtn);
}

VOID Instruction(INS ins, void * v)
{
    if ( INS_IsStackWrite(ins) )
    {
        // map sparse INS addresses to dense IDs
        //const ADDRINT iaddr = INS_Address(ins);
            
        const UINT32 size = INS_MemoryWriteSize(ins);

        const BOOL   single = (size <= 4);
                
	if( single )
	{
		INS_InsertPredicatedCall(
			ins, IPOINT_BEFORE,  (AFUNPTR) StoreSingle,
			IARG_MEMORYWRITE_EA,
			IARG_ADDRINT, INS_Address(ins),
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
	
	// record the count of function entry and exit via "CALL" and "Execution of the return address-instruction" 
	// assume that the entry instruction will be executed once within each function instance
	INS_InsertPredicatedCall(
		ins, IPOINT_BEFORE,  (AFUNPTR) AfterCall,		
		IARG_ADDRINT, INS_Address(ins),
		IARG_END);

	if( INS_Opcode(ins) == XED_ICLASS_CALL_NEAR )
	{	
		ADDRINT nextAddr = INS_NextAddress(ins);
		//cerr << hex << nextAddr;
		//ADDRINT callee = INS_DirectBranchOrCallTargetAddress(ins);
		//cerr << "->" << callee << endl;	
	
		INS_InsertPredicatedCall(
			ins, IPOINT_BEFORE,  (AFUNPTR) BeforeCall,
			IARG_ADDRINT, nextAddr,				
			IARG_BRANCH_TARGET_ADDR,				
			IARG_END);		
	
	}	
		
}

/* ===================================================================== */

VOID Fini(int code, VOID * v)
{
    // print D-cache profile
    // @todo what does this print
    
    g_outFile << "PIN:MEMLATENCIES 1.0. 0x0\n";
            
    g_outFile <<
        "#\n"
        "# Write stats with cell size:\t";
	g_outFile << (1<<g_nProfDistPower);
	g_outFile <<	"\n\n";
	g_outFile << "Total function counts:\t" << g_nFuncCount << endl;
	
	set<UINT64> zFrames;
	std::list<pair<UINT64, bool> >::iterator I = g_InstanceTrace.begin(), E = g_InstanceTrace.end();
	for(; I != E; ++ I)
	{
		UINT64 funcInst = I->first;
		bool isEntry = I->second;
		UINT64 funcAddr = g_hFuncInst2Addr[funcInst];
		//cerr << funcAddr << endl;

		//string szFunc = g_hAddr2Func[funcAddr];

		UINT32 stackSize = g_hFunc2StackSize[funcAddr];
		UINT64 nWrites = g_hFuncInst2W[funcInst];
		// erase the frames with no write to reduce the total number of frames to speed the evaluation
				
		if( isEntry)
		{
			if( nWrites == 3 )
			{
				zFrames.insert(funcInst);				
				continue;
			}
			g_outFile << hex << ":" << funcInst << endl << dec;
		}
		else
		{
			if( zFrames.find(funcInst) != zFrames.end() )
			{				
				continue;
			}
			g_outFile  <<hex << funcInst << "\t" << stackSize << "\t" << nWrites << endl << dec;
		}
	}  
    g_outFile.close();
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
	g_nProfDistPower = KnobProfDistance.Value();
	char buf[256];
    sprintf(buf, "%u",g_nProfDistPower);
    string szOutFile = KnobOutputFile.Value() +"_" + buf;

    g_outFile.open(szOutFile.c_str());

    

    RTN_AddInstrumentFunction(Routine, 0);    
    INS_AddInstrumentFunction(Instruction, 0);

    PIN_AddFiniFunction(Fini, 0);

    // Never returns

    PIN_StartProgram();
    
    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
