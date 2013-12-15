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
 *  This file collects the stack frame trace as well as the dynamic allocation trace.
 *  1. For dynamic allocation trace:
 *  1) The "alloc" class functions are instrumented to track the block trace, as well its size and address (size per block)
 *  2) The memory write operations are tracked and analyzed to determine the target block (writes per block)
 * 
 *  2. For stack frame trace:
 *  1) The "call" instruction and the next instruction (function return address) are instrumented to track the frame trace 
 *  2) The "SUB/ADD ESP" instructions are tracked to determine the frame size (size per frame)
 *  3) The stack write operations are tracked and analyzed to determine the target frame (writes per frame)
 *
 *  3. To collect the stack frame trace, enable the STACK macro
    To collect the heap object trace, enable the HEAP macro
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
#define ZERO_STACK
#define STACK
//#define HEAP

/* ===================================================================== */
/* Names of malloc and free */
/* ===================================================================== */
#if defined(TARGET_MAC)
#define MALLOC "_malloc"
#define FREE "_free"
#else
#define MALLOC "__libc_malloc"
#define FREE "__cfree"
#endif

/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE,    "pintool",
    "o", "stack.out", "specify dcache file name");
KNOB<UINT32> KnobProfDistance(KNOB_MODE_WRITEONCE, "pintool",
    "d","5", "profing distance power: n -> 2^n");



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

UINT32 g_nProfDistPower = 5;
std::ofstream g_outFile;


UINT64 g_nFrameCount = 0;		  // unique ID for each frame
UINT64 g_nAllocCount = 0;		// unique ID for each dynamic allocation

std::list<UINT64> g_FrameStack;   // the frame stack
std::list<UINT64> g_RetStack; // stack for return addresses

std::list<pair<UINT64, bool> > g_FrameTrace; 

// storage for efficiency
std::map<ADDRINT, string> g_hAddr2Name;

std::map<UINT64, UINT64> g_hFrame2W; // map frame to number of writes
//std::map<UINT64, UINT64> g_hFrame2Func; // map frame to function address
std::map<UINT64, UINT32> g_hFrame2Size;
std::map<ADDRINT, UINT32> g_hFunc2FrameSize; // map function address to stack size

// evaluation for motivation
std::map<ADDRINT, UINT64> g_hLine2W;
std::map<ADDRINT, std::set<UINT64> > g_hLine2Frames;
//std::map<ADDRINT, std::set<UINT64> > g_hLine2Blocks;

// switch to control the profiling
bool g_bEnable = false;


// for heap area
std::map<ADDRINT, UINT64> g_Heap;

//std::map<UINT64, UINT32> g_hBlock2Size;
//std::map<UINT64, UINT64> g_hBlock2W;



/* ===================================================================== */
VOID DumpSpace(UINT32 num)
{
	for(UINT32 i = 0; i< num; ++ i)
		cerr << ' ';
}



/* ===================================================================== */
/* Analysis routines for heap objects                                               */
/* ===================================================================== */
 
ADDRINT g_nCurrentBlockSize = 0;
VOID AllocSize(ADDRINT size)
{
	//cerr << hex << "(" << size << ")" << dec << endl;
	g_nCurrentBlockSize = size;
}

VOID AllocAddress(ADDRINT startAddr)
{
	++ g_nFrameCount;			
	g_hFrame2Size[g_nFrameCount] = g_nCurrentBlockSize;	
	g_FrameTrace.push_back(pair<UINT64, bool>(g_nFrameCount, true) );	
	
	
	ADDRINT endAddr = startAddr + g_nCurrentBlockSize - 1;
	ADDRINT line1 = startAddr >> g_nProfDistPower;		// glibc aligns each heap block with 16 (2^4) bytes
	ADDRINT line2 = endAddr >> g_nProfDistPower;
	for(ADDRINT i = line1; i <= line2; ++ i )
	{
		g_Heap[i] = g_nFrameCount;
		g_hLine2Frames[i].insert(g_nFrameCount);
	}
}

VOID DeallocAddress(ADDRINT startAddr)
{
	if( startAddr == 0 )
		return;
    //cerr << "  returns " <<hex << ret << dec << endl;
	g_FrameTrace.push_back(pair<UINT64, bool>(g_nFrameCount, false) );	
	
	// This is redundant. Since without memory error, there heap write is only on valid addresses 
	//ADDRINT line1 = startAddr >> g_nProfDistPower;
	//g_Heap[line1]->_bValid = false;     
}

/* ===================================================================== */
/* Analysis routines for stack frames                                               */
/* ===================================================================== */
VOID CallBegin(ADDRINT nextAddr, ADDRINT callee )
{
#ifdef ONLY_MAIN	
	string szFunc = g_hAddr2Name[callee];
	if( szFunc == "main")
		g_bEnable = true;

	if( !g_bEnable)
		return;
#endif
	
	//DumpSpace(g_instanceStack.size());	
	
	++ g_nFrameCount;	
	//g_hFrame2Func[g_nFrameCount] = callee;	
	

	g_hFrame2Size[g_nFrameCount] = g_hFunc2FrameSize[callee];		
	g_FrameStack.push_back(g_nFrameCount);	
	g_RetStack.push_back(nextAddr);

#ifdef ZERO_STACK	
	if( g_hFrame2Size[g_nFrameCount] != 0 )
#endif
	{
		//cerr << hex << nextAddr << "--calling " << callee << endl;
		g_FrameTrace.push_back(pair<UINT64, bool>(g_nFrameCount, true) );
	}	
}

VOID CallEnd(ADDRINT iAddr )
{
	if( g_RetStack.empty() )
		return;

	//DumpSpace(g_instanceStack.size());	
	
	
	if( iAddr == g_RetStack.back() )  // to identify a function return address
	{
		UINT64 frame = g_FrameStack.back();
#ifdef ZERO_STACK	
		if( g_hFrame2Size[frame] != 0 )
#endif
		{			
			g_FrameTrace.push_back(pair<UINT64, bool>(frame, false) );
		}

#ifdef ONLY_MAIN	
		ADDRINT fAddr = g_hFrame2Func[g_FrameStack.back()];
		string szFunc = g_hAddr2Name[fAddr];
		if( szFunc == "main")
			g_bEnable = false;

		if( !g_bEnable)
			return;
#endif
		g_FrameStack.pop_back();
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

	UINT64 frame = g_FrameStack.back();
#ifdef ZERO_STACK	
	if( g_hFrame2Size[frame] == 0 )
		return;
#endif
	
	UINT64 line = addr >> g_nProfDistPower;
	if( line < 0x400000 )
		return;

	UINT64 nSize = size >> 3; 			// 64-bit memory width, which equals 2^3=8 bytes

	
	g_hFrame2W[frame] += nSize;
	
    g_hLine2W[line] += nSize;	
	g_hLine2Frames[line].insert( frame );	
}

/* ===================================================================== */

VOID StoreSingle(ADDRINT addr, ADDRINT iaddr)
{
#ifdef ONLY_MAIN
	if(!g_bEnable)
		return;
#endif	

	UINT64 frame = g_FrameStack.back();
#ifdef ZERO_STACK	
	if( g_hFrame2Size[frame] == 0 )
		return;
#endif

	UINT64 line = addr >> g_nProfDistPower;
	if( line < 0x400000 )
		return;
	
	++ g_hFrame2W[frame];

	//UINT64 line = addr >> g_nProfDistPower;
	
    ++ g_hLine2W[line];	
	g_hLine2Frames[line].insert(frame );	
}

VOID StoreMultiH(ADDRINT addr, UINT64 size)
{
#ifdef ONLY_MAIN
	if(!g_bEnable)
		return;
#endif
	//UINT64 line = addr >> g_nProfDistPower;	
	UINT64 line = addr >> g_nProfDistPower;
	if( line < 0x400000 )
		return;

	if( g_Heap.find(line) == g_Heap.end() )
		return;

	UINT64 nSize = size >> 3; 		// 64-bit memory width, which equals 2^3=8 bytes

	g_hLine2W[line] += nSize;
	ADDRINT nBlock = g_Heap[line];
	++ g_hFrame2W[nBlock];	

	g_hLine2Frames[line].insert(nBlock);
}

/* ===================================================================== */

VOID StoreSingleH(ADDRINT addr, ADDRINT iaddr)
{
#ifdef ONLY_MAIN
	if(!g_bEnable)
		return;
#endif	
	UINT64 line = addr >> g_nProfDistPower;

	if( g_Heap.find(line) == g_Heap.end() )
		return;	
	
	++ g_hLine2W[line];

	ADDRINT nBlock = g_Heap[line];
	++ g_hFrame2W[nBlock];	

	g_hLine2Frames[line].insert(nBlock);
}
/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */
   
VOID Image(IMG img, VOID *v)
{

#ifdef HEAP
    // Instrument the malloc() and free() functions.  Print the input argument
    // of each malloc() or free(), and the return value of malloc().
    //
    // 1. Find the malloc() function.
    RTN mallocRtn = RTN_FindByName(img, MALLOC);
    if (RTN_Valid(mallocRtn))
    {
        RTN_Open(mallocRtn);

        // Instrument malloc() to print the input argument value and the return value.
        RTN_InsertCall(mallocRtn, IPOINT_BEFORE, (AFUNPTR)AllocSize,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                       IARG_END);
        RTN_InsertCall(mallocRtn, IPOINT_AFTER, (AFUNPTR)AllocAddress,
                       IARG_FUNCRET_EXITPOINT_VALUE, IARG_END);

        RTN_Close(mallocRtn);
    }

    // 2. Find the free() function.
    RTN freeRtn = RTN_FindByName(img, FREE);
    if (RTN_Valid(freeRtn))
    {
        RTN_Open(freeRtn);
        // Instrument free() to print the input argument value.
        RTN_InsertCall(freeRtn, IPOINT_BEFORE, (AFUNPTR)DeallocAddress,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                       IARG_END);
        RTN_Close(freeRtn);
    }
#endif

#ifdef STACK
	// 3. Visit all routines to collect frame size
	for( SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec) )
	{
		for( RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn) )
		{
			RTN_Open(rtn);

			// collect user functions as well as their addresses
			ADDRINT fAddr = RTN_Address(rtn);	
			string szFunc = RTN_Name(rtn);
			g_hAddr2Name[fAddr] = szFunc;
			//cerr << fAddr << ":\t" << szFunc << endl;
			//bool bFound = false;
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
					g_hFunc2FrameSize[fAddr] = nOffset;	             
					//bFound = true;
					break;           
				}
				if( INS_Opcode(ins) == XED_ICLASS_ADD && 
					INS_OperandIsReg(ins, 0 ) && INS_OperandReg(ins, 0) == REG_STACK_PTR  &&
					INS_OperandIsImmediate(ins, 1) )
				{
					int nOffset = INS_OperandImmediate(ins, 1);
					if(nOffset < 0 )
					{
						nOffset = -nOffset;
					}           	
					g_hFunc2FrameSize[fAddr] = nOffset;	  
					//bFound = true;
					break;
				}		
			}
			//if( !bFound )
			//	g_hFunc2FrameSize[fAddr] = 0;
			RTN_Close(rtn);
		}
	}
#endif
}

/* ===================================================================== */


VOID Instruction(INS ins, void * v)
{

	// track the write operations
    if ( INS_IsStackWrite(ins) )
    {
        // map sparse INS addresses to dense IDs
        //const ADDRINT iaddr = INS_Address(ins);
#ifdef STACK            
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
#endif
		;       
    }



	else if( INS_IsMemoryWrite(ins) )
	{
#ifdef HEAP
		const UINT32 size = INS_MemoryWriteSize(ins);

        const BOOL   single = (size <= 4);
                
		if( single )
		{
			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE,  (AFUNPTR) StoreSingleH,
				IARG_MEMORYWRITE_EA,
				IARG_ADDRINT, INS_Address(ins),
				IARG_END);
		}
		else
		{
			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE,  (AFUNPTR) StoreMultiH,
				IARG_MEMORYWRITE_EA,
				IARG_MEMORYWRITE_SIZE,
				IARG_END);
		}	
#endif
		;		
	}

	
#ifdef STACK
	// track the frame allocation/deallocation
	// record the count of function entry and exit via "CALL" and "Execution of the return address-instruction" 
	// assume that the entry instruction will be executed once within each frame
	INS_InsertPredicatedCall(
		ins, IPOINT_BEFORE,  (AFUNPTR) CallEnd,		
		IARG_ADDRINT, INS_Address(ins),
		IARG_END);

	if( INS_Opcode(ins) == XED_ICLASS_CALL_NEAR )
	{	
		ADDRINT nextAddr = INS_NextAddress(ins);
		//cerr << hex << nextAddr;
		//ADDRINT callee = INS_DirectBranchOrCallTargetAddress(ins);
		//cerr << "->" << callee << endl;	
	
		INS_InsertPredicatedCall(
			ins, IPOINT_BEFORE,  (AFUNPTR) CallBegin,
			IARG_ADDRINT, nextAddr,				
			IARG_BRANCH_TARGET_ADDR,				
			IARG_END);		
	
	}	
#endif		
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
	g_outFile << "Total function counts:\t" << g_nFrameCount << endl;	
	
	std::list<pair<UINT64, bool> >::iterator I = g_FrameTrace.begin(), E = g_FrameTrace.end();
	for(; I != E; ++ I)
	{
		UINT64 Frame = I->first;
		bool isEntry = I->second;
		
		UINT32 size = g_hFrame2Size[Frame];
		UINT64 nWrites = g_hFrame2W[Frame];
		// erase the frames with no write to reduce the total number of frames to speed the evaluation
		if( nWrites == 0 )
		{						
			continue;
		}
				
		if( isEntry)
		{			
			g_outFile << hex << ":" << Frame << "\t" << dec << size << "\t" << nWrites << endl << dec; 
		}
		else
		{			
			g_outFile  <<hex << Frame << endl << dec;
		}
	}  
    g_outFile.close();

	
	g_outFile.open("stack.out_5_23_2");
	std::map<ADDRINT, UINT64>::iterator i2i_p = g_hLine2W.begin(), i2i_e = g_hLine2W.end();
	for(; i2i_p != i2i_e; ++ i2i_p)
	{
		g_outFile << hex << (i2i_p->first << g_nProfDistPower) << "\t" <<dec << g_hLine2Frames[i2i_p->first].size() << "\t" << (double)i2i_p->second << endl;
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

    
    IMG_AddInstrumentFunction(Image, 0);     
    INS_AddInstrumentFunction(Instruction, 0);

    PIN_AddFiniFunction(Fini, 0);

    // Never returns

    PIN_StartProgram();
    
    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
