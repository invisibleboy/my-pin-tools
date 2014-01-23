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
 *  This file is for evaluation for volatile PCM.
 *  1) It collects write-energy for each 32-bit PCM block for endurance evaluation
 *  2) It collects write-interval distribution for the motivation
 *  3) It collects write-mode distribution for write-mode-selection motivation, in the Second Time
 */


#include "pin.H"

#include <iostream>
#include <fstream>
#include <map>
#include <sstream>
#include <set>

#define SecondTime

#define MACHINE32


/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE,    "pintool",
    "o", "irw.out", "specify dcache file name");


/* ===================================================================== */
/* Global variables */
/* ===================================================================== */
#ifdef USER_PROFILE
bool g_bStartSimu = false;	
#endif

const UINT64 C_i = 1;
const UINT64 C_r = 1;
const UINT32 C_fastW = 3;
const UINT32 C_slowW = 10;
const double E_r = 3.0*16;
const double E_fastW = (19.2 + 13.5*3)*16;
const double E_slowW = (19.2 + 7.72*10)*16;
ADDRINT g_nClock = 0;
UINT64 g_nRefresh = 0;



//std::map<UINT32, UINT64> g_hInterval;
UINT64 g_nTotalRead = 0;
UINT64 g_nTotalInst = 0;
UINT64 g_nTotalWrite = 0;
UINT64 g_nTotalWriteL;
UINT64 g_nTotalWriteLstatic;
std::map<ADDRINT, UINT64> g_hWriteInstL;
std::map<ADDRINT, bool> g_hWriteInstIsL;
std::map<ADDRINT, UINT64> g_hLastR;
std::map<ADDRINT, UINT64> g_hLastW;
std::map<ADDRINT, ADDRINT> g_hMem2InstW;

std::map<ADDRINT, UINT64> g_hFastW;  	// for 1)
std::map<ADDRINT, UINT64> g_hSlowW;
std::map<ADDRINT, UINT64> g_hIdealSlowW;
std::map<ADDRINT, UINT64> g_hIdealFastW;

UINT64 g_nTotalInterval = 0;             	// for 2)
std::map<UINT32, UINT64> g_hInterval;

UINT64	g_nFastCount = 0;					// for 3)
UINT64 g_nTotalFast = 0;
std::map<UINT32, UINT64> g_hFastCount;

std::set<ADDRINT> g_footprint;
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

UINT32 OrderNum(ADDRINT interval, UINT32 granu)
{
	if( interval == 0)
		return 0;
		
	UINT32 order = 0;
	do
	{
		interval = interval >> granu;   // 4^
		++ order;
	}while( interval > 0);
	return order;
}
/* ===================================================================== */

VOID LoadMulti(ADDRINT addr, UINT32 size)
{
	
	
	size = size/4;
#ifdef MACHINE32
	addr = addr & 0xfffffffc;
#else
	addr = addr & 0xfffffffffffffffcLLU;
#endif
	//cerr << "L: " << addr << "-" << g_nClock << endl;
    // first level D-cache

	g_nTotalRead += size;
	g_nClock += size * C_r;	
	for( UINT32 i = 0; i < size; ++ i)
	{
		ADDRINT addr1 = addr + i;
		g_footprint.insert(addr1);
	}
}

/* ===================================================================== */

VOID StoreMulti(ADDRINT iAddr, ADDRINT addr, UINT32 size)
{
	size = (size+3)/4;
#ifdef MACHINE32
	addr = addr & 0xfffffffc;
#else
	addr = addr & 0xfffffffffffffffcLLU;
#endif
	
	//cerr << "L: " << addr << "-" << g_nClock << endl;

	g_nTotalWrite += size;
	g_nClock += C_fastW * size;	
	for( UINT32 i = 0; i < size; ++ i)
	{
		ADDRINT addr1 = addr + i;
		g_footprint.insert(addr1);
	}
}

/* ===================================================================== */

VOID LoadSingle(ADDRINT addr)
{
	++ g_nTotalRead;
	g_nClock += C_r;			// 1-clock per read
	g_footprint.insert(addr);
#ifdef MACHINE32
	addr = addr & 0xfffffffc;
#else
	addr = addr & 0xfffffffffffffffcLLU;
#endif
	//cerr << "L: " << addr << "-" << g_nClock << endl;
	//cerr << "L: " << addr << "-" << g_nClock << endl;
}
/* ===================================================================== */

VOID StoreSingle(ADDRINT iAddr, ADDRINT addr)
{	
	++ g_nTotalWrite;
	g_nClock += C_fastW;			// 3-clock per fast-write
	g_footprint.insert(addr);
#ifdef MACHINE32
	addr = addr & 0xfffffffc;
#else
	addr = addr & 0xfffffffffffffffcLLU;
#endif	
}
/* ===================================================================== */
VOID ExecInst()
{
	++ g_nTotalInst;
	g_nClock += C_i;
}

/* ===================================================================== */
/*VOID Image(IMG img, void *v)
{
	// control the simulation 
	RTN rtn = RTN_FindByName(img, "main");
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)SetSimu, IARG_END);
	RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)UnsetSimu, IARG_END);
	RTN_Close(rtn);
}*/


VOID Routine(RTN rtn, void *v)
{
#ifdef USER_PROFILE	
// control the simulation 
	string szFunc = RTN_Name(rtn);
	if(szFunc == "main")
	{
		RTN_Open(rtn);
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)SetSimu, IARG_END);
		RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)UnsetSimu, IARG_END);
		RTN_Close(rtn);	
	}
#endif
}


/* ===================================================================== */
VOID Instruction(INS ins, void * v)
{
	INS_InsertPredicatedCall(
		ins, IPOINT_BEFORE, (AFUNPTR) ExecInst,
		IARG_END);
	
	ADDRINT iaddr = INS_Address(ins);
	
    if (INS_IsMemoryRead(ins))
    {
        const UINT32 size = INS_MemoryReadSize(ins);
        const BOOL   single = (size <= 4);

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
        const UINT32 size = INS_MemoryWriteSize(ins);

        const BOOL   single = (size <= 4);
		if( single )
		{

			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE,  (AFUNPTR) StoreSingle,
				IARG_ADDRINT, iaddr,
				IARG_MEMORYWRITE_EA,
				IARG_END);

		}
		else
		{
			INS_InsertPredicatedCall(
				ins, IPOINT_BEFORE,  (AFUNPTR) StoreMulti,
				IARG_ADDRINT, iaddr,
				IARG_MEMORYWRITE_EA,
				IARG_MEMORYWRITE_SIZE,
				IARG_END);
		} 
	}

   
}

/* ===================================================================== */

VOID Fini(int code, VOID * v)
{

	//char buf[256];
	//sprintf(buf, "%llu", R_fastW);
	
	string szOutFile = KnobOutputFile.Value() ;
    std::ofstream out(szOutFile.c_str());

    out << "PIN:MEMLATENCIES 1.0. 0x0\n";
            
    out <<
        "#\n"
        "# IRW stats\n"
        "#\n";    	
	out << "Total Instruction:\t" << g_nTotalInst << endl;
	out << "Total Reads:\t" << g_nTotalRead << endl;
	out << "Total Writes:\t" << g_nTotalWrite << endl;    
	out << "Total fast-clock:\t" << g_nClock << endl;
	out << "Total footprint in Byte:\t" << g_footprint.size() * 4 << endl;
      
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
  
    
    INS_AddInstrumentFunction(Instruction, 0);
#ifdef USER_PROFILE
	RTN_AddInstrumentFunction(Routine, 0);
#endif
	//IMG_AddInstrumentFunction( Image, 0);
    PIN_AddFiniFunction(Fini, 0);

    // Never returns

    PIN_StartProgram();

    
    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
