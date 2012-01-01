/* Classic Ladder Project */
/* Copyright (C) 2001-2006 Marc Le Douarain */
/* http://www.multimania.com/mavati/classicladder */
/* http://www.sourceforge.net/projects/classicladder */
/* February 2001 */
/* --------------------------- */
/* Alloc/free global variables */
/* --------------------------- */
/* This library is free software; you can redistribute it and/or */
/* modify it under the terms of the GNU Lesser General Public */
/* License as published by the Free Software Foundation; either */
/* version 2.1 of the License, or (at your option) any later version. */

/* This library is distributed in the hope that it will be useful, */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU */
/* Lesser General Public License for more details. */

/* You should have received a copy of the GNU Lesser General Public */
/* License along with this library; if not, write to the Free Software */
/* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef MODULE
#include <stdio.h>
#include <stdlib.h>
#endif

#include "rtapi.h"
#include "rtapi_string.h"

#include "classicladder.h"
#include "files.h"
#include "calc.h"
#include "vars_access.h"
#include "manager.h"
#include "calc_sequential.h"
#include "symbols.h"

#if defined( MODULE )
#include <linux/string.h>
extern void CopySizesInfosFromModuleParams( void );
#else
#include <string.h>
#endif

#ifdef GTK_INTERFACE
#include "classicladder_gtk.h"
#include "manager_gtk.h"
#include "symbols_gtk.h"
//#include <gtk/gtk.h>
#endif

#ifdef MAT_CONNECTION
#include "../../lib/plc.h"
#endif


#ifndef HAL_SUPPORT
#if defined( RTLINUX ) || defined ( __RTL__ )
#include "/usr/rtlinux/include/mbuff.h"
#endif
#endif

#ifdef HAL_SUPPORT
#include "rtapi.h"
#include "hal.h"
#define CL_SHMEM_KEY 0x434C522b // "CLR+"
int compId;
static int ShmemId;
#endif

#if defined(RTAI)
#if !defined(MODULE)
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include "rtai.h"
#include "rtai_shm.h"
#define mbuff_alloc(a, b) rtai_malloc(nam2num(a), b)
#define mbuff_free(a, b) rtai_free(nam2num(a), b)
#else
#include <linux/module.h>
#include "rtai.h"
#include "rtai_shm.h"
#define mbuff_alloc(a, b) rtai_kmalloc(nam2num(a), b)
#define mbuff_free(a, b) rtai_kfree(nam2num(a))
#endif
#endif

StrRung * RungArray;
TYPE_FOR_BOOL_VAR * VarArray;
int * VarWordArray;
StrTimer * TimerArray;
StrMonostable * MonostableArray;
StrCounter * CounterArray;
StrArithmExpr * ArithmExpr;
StrInfosGene * InfosGene;
StrSection * SectionArray;
#ifdef SEQUENTIAL_SUPPORT
StrSequential * Sequential;
#endif
StrSymbol * SymbolArray;

#ifdef GTK_INTERFACE
/* used for the editor */
StrEditRung EditDatas;
StrArithmExpr * EditArithmExpr;
#endif


//#ifdef DYNAMIC_PLCSIZE
//plc_sizeinfo_s	*plc_sizeinfo;
//#endif

// Defaults sizes values
#ifdef DYNAMIC_PLCSIZE
plc_sizeinfo_s sinfo = {
	.nbr_rungs = NBR_RUNGS_DEF,
	.nbr_bits = NBR_BITS_DEF,
	.nbr_words = NBR_WORDS_DEF,
	.nbr_timers = NBR_TIMERS_DEF,
	.nbr_monostables = NBR_MONOSTABLES_DEF,
	.nbr_counters = NBR_COUNTERS_DEF,
	.nbr_phys_inputs = NBR_PHYS_INPUTS_DEF,
	.nbr_phys_outputs = NBR_PHYS_OUTPUTS_DEF,
	.nbr_arithm_expr = NBR_ARITHM_EXPR_DEF,
	.nbr_sections = NBR_SECTIONS_DEF,
	.nbr_symbols = NBR_SYMBOLS_DEF,
	.nbr_s32in = NBR_S32IN_DEF,
	.nbr_s32out = NBR_S32OUT_DEF
};
#endif


#ifdef GTK_INTERFACE
char LadderDirectory[400] = "projects_examples/example.clp";
#else
char LadderDirectory[400] = "projects_examples/parallel_port_test.clp";
#endif
char TmpDirectory[ 400 ];

/* return TRUE if okay */
int ClassicLadderAllocAll()
{
    unsigned char *pByte; 
    unsigned long bytes = sizeof(StrInfosGene) + sizeof(long);
    unsigned long *shmBase;
#ifdef RTAPI
    int numBits, numWords;
#endif
    plc_sizeinfo_s *pSizesInfos;
    
#ifdef RTAPI
    pSizesInfos = &sinfo;
    // Calculate SHMEM size.
    numBits =
        pSizesInfos->nbr_bits + pSizesInfos->nbr_phys_inputs +
        pSizesInfos->nbr_phys_outputs;
    numWords = pSizesInfos->nbr_words;
#ifdef SEQUENTIAL_SUPPORT
    numBits += NBR_STEPS;
    numWords += NBR_STEPS;
#endif
    bytes += pSizesInfos->nbr_rungs * sizeof(StrRung);
    bytes += pSizesInfos->nbr_timers * sizeof(StrTimer);
    bytes += pSizesInfos->nbr_monostables * sizeof(StrMonostable);
    bytes += pSizesInfos->nbr_arithm_expr * sizeof(StrArithmExpr);
    bytes += pSizesInfos->nbr_sections * sizeof(StrSection);
    bytes += pSizesInfos->nbr_symbols * sizeof(StrSymbol);
    bytes += pSizesInfos->nbr_counters * sizeof(StrCounter);
#ifdef SEQUENTIAL_SUPPORT
    bytes += sizeof(StrSequential);
#endif
    bytes += numWords * sizeof(int);
    bytes += numBits * sizeof(TYPE_FOR_BOOL_VAR);
#endif
     
    // Attach SHMEM with proper size.
    if ((ShmemId = rtapi_shmem_new(CL_SHMEM_KEY, compId, bytes)) < 0) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Failed to alloc shared memory (%x %d %lu) !\n",
                CL_SHMEM_KEY, compId, bytes);
        return FALSE;
    }
    rtapi_print_msg(RTAPI_MSG_INFO, "Shared memory: %x %d %lu\n",
                CL_SHMEM_KEY, compId, bytes);
    // Map SHMEM.
    if (rtapi_shmem_getptr(ShmemId, (void **) &shmBase) < 0) {
        rtapi_print("Failed to map shared memory !\n");
        return FALSE;
    }
#ifndef RTAPI
    // Check signature written by RT module to make sure we have the
    // right region and RT module is loaded.
    if (shmBase[0] != CL_SHMEM_KEY) {
        rtapi_print("Shared memory conflict or RT component not loaded!\n");
        return FALSE;
    }
    bytes = shmBase[1];

    rtapi_shmem_delete(CL_SHMEM_KEY, compId);

    InfosGene = (StrInfosGene*)(shmBase+1);
    pSizesInfos = &(InfosGene->SizesInfos);
#else
    // Initialize SHMEM.
    shmBase[0] = CL_SHMEM_KEY;
    shmBase[1] = bytes;
    InfosGene = (StrInfosGene*)(shmBase+1);
    InfosGene->SizesInfos = *pSizesInfos;

    InfosGene->LadderState = STATE_LOADING;
    InfosGene->CmdRefreshVarsBits = FALSE;

    InfosGene->BlockWidth = BLOCK_WIDTH_DEF;
    InfosGene->BlockHeight = BLOCK_HEIGHT_DEF;
    InfosGene->PageWidth = 0;
    InfosGene->PageHeight = 0;
    InfosGene->TopRungDisplayed = 0;
    InfosGene->OffsetHiddenTopRungDisplayed = 0;
    InfosGene->OffsetCurrentRungDisplayed = 0;
    InfosGene->VScrollValue = 0;
    InfosGene->HScrollValue = 0;

    InfosGene->DurationOfLastScan = 0;
    InfosGene->CurrentSection = 0;
#endif

     rtapi_print_msg(RTAPI_MSG_INFO, "Sizes: rungs- %d bits- %d words- %d timers- %d mono- %d count- %d \n HAL Bin- %d HAL Bout- %d expressions- %d sections- %d symbols - %d\n s32in - %d s32out- %d\n",
        pSizesInfos->nbr_rungs,
        pSizesInfos->nbr_bits,
        pSizesInfos->nbr_words,
        pSizesInfos->nbr_timers,
        pSizesInfos->nbr_monostables,
        pSizesInfos->nbr_counters,
        pSizesInfos->nbr_phys_inputs,
        pSizesInfos->nbr_phys_outputs,
        pSizesInfos->nbr_arithm_expr,
        pSizesInfos->nbr_sections,
        pSizesInfos->nbr_symbols,
	    pSizesInfos->nbr_s32in,
	    pSizesInfos->nbr_s32out);

    // Set global SHMEM pointers.
    pByte = (unsigned char *) InfosGene;
    pByte += sizeof(StrInfosGene);

    RungArray = (StrRung *) pByte;
    pByte += pSizesInfos->nbr_rungs * sizeof(StrRung);

    TimerArray = (StrTimer *) pByte;
    pByte += pSizesInfos->nbr_timers * sizeof(StrTimer);

    MonostableArray = (StrMonostable *) pByte;
    pByte += pSizesInfos->nbr_monostables * sizeof(StrMonostable);

    ArithmExpr = (StrArithmExpr *) pByte;
    pByte += pSizesInfos->nbr_arithm_expr * sizeof(StrArithmExpr);

    SectionArray = (StrSection *) pByte;
    pByte += pSizesInfos->nbr_sections * sizeof(StrSection);

    SymbolArray = (StrSymbol *) pByte;
    pByte += pSizesInfos->nbr_symbols * sizeof(StrSymbol);

    CounterArray = (StrCounter *) pByte;
    pByte += pSizesInfos->nbr_counters * sizeof(StrCounter);

#ifdef SEQUENTIAL_SUPPORT
    Sequential = (StrSequential *) pByte;
    rtapi_print_msg(RTAPI_MSG_INFO, "Sequential: %p\n", Sequential);
    pByte += sizeof(StrSequential);
#endif

    VarWordArray = (int *) pByte;
    pByte += SIZE_VAR_WORD_ARRAY * sizeof(int);

    // Allocate last for alignment reasons.
    VarArray = (TYPE_FOR_BOOL_VAR *) pByte;

    rtapi_print_msg(RTAPI_MSG_INFO, "VarArray = %p (%ld)\n", VarArray, (long)(pByte - (unsigned char*)shmBase));

#ifdef GTK_INTERFACE
	EditArithmExpr = (StrArithmExpr *)malloc( NBR_ARITHM_EXPR * sizeof(StrArithmExpr) );
	if (!EditArithmExpr)
	{
		rtapi_print_msg(RTAPI_MSG_ERR, "Failed to alloc EditArithmExpr !\n");
		return FALSE;
	}
#endif

/*#ifdef MODULE
rtl_printf("Allocated %d rungs, %d timers, %d monostables...\n", NBR_RUNGS, NBR_TIMERS, NBR_MONOSTABLES);
#else
printf("Allocated %d rungs, %d timers, %d monostables...\n", NBR_RUNGS, NBR_TIMERS, NBR_MONOSTABLES);
#endif*/

	InfosGene->LadderState = STATE_LOADING;
	InfosGene->CmdRefreshVarsBits = FALSE;

	InfosGene->BlockWidth = BLOCK_WIDTH_DEF;
	InfosGene->BlockHeight = BLOCK_HEIGHT_DEF;
	InfosGene->PageWidth = 0;
	InfosGene->PageHeight = 0;
	InfosGene->TopRungDisplayed = 0;
	InfosGene->OffsetHiddenTopRungDisplayed = 0;
	InfosGene->OffsetCurrentRungDisplayed = 0;
	InfosGene->VScrollValue = 0;
	InfosGene->HScrollValue = 0;



	InfosGene->MsSinceLastScan = 0;
	InfosGene->NsSinceLastScan = 0;

	InfosGene->DurationOfLastScan = 0;
	InfosGene->CurrentSection = 0;
	InitIOConf( );
	InfosGene->AskConfirmationToQuit = FALSE;
	InfosGene->DisplaySymbols = TRUE;

	return TRUE;
}

void ClassicLadderFreeAll()
{
#ifdef GTK_INTERFACE
	if (EditArithmExpr)
		free(EditArithmExpr);
#endif
#ifdef HAL_SUPPORT
	rtapi_shmem_delete(ShmemId, compId);
		
#elif !defined(RT_SUPPORT)
	if (RungArray)
		free(RungArray);
	if (TimerArray)
		free(TimerArray);
	if (MonostableArray)
		free(MonostableArray);
	if (CounterArray)
		free(CounterArray);
	if (VarArray)
		free(VarArray);
	if (VarWordArray)
		free(VarWordArray);
	if (ArithmExpr)
		free(ArithmExpr);
	if (InfosGene)
		free(InfosGene);
	if (SectionArray)
		free(SectionArray);
#ifdef SEQUENTIAL_SUPPORT
	if (Sequential)
		free(Sequential);
#endif
	if (SymbolArray)
		free(SymbolArray);
	CleanTmpDirectory( TmpDirectory, TRUE/*DestroyDir*/ );
#else
	if (RungArray)
		mbuff_free("Rungs",RungArray);
	if (TimerArray)
		mbuff_free("Timers",TimerArray);
	if (VarArray)
		mbuff_free("VarsBits",VarArray);
	if (MonostableArray)
		mbuff_free("Monostables",MonostableArray);
	if (CounterArray)
		mbuff_free("Counters",CounterArray);
	if (VarWordArray)
		mbuff_free("VarWords",VarWordArray);
	if (ArithmExpr)
		mbuff_free("ArithmExpr",ArithmExpr);
	if (InfosGene)
		mbuff_free("InfosGene",InfosGene);
	if (SectionArray)
		mbuff_free("Sections",SectionArray);
#ifdef SEQUENTIAL_SUPPORT
	if (Sequential)
		mbuff_free("Sequential",Sequential);
#endif
	if (SymbolArray)
		mbuff_free("Symbols",SymbolArray);
#endif
}

void InitAllLadderDatas( char NoScreenRefresh )
{
	InitVars();
	InitTimers();
	InitMonostables();
	InitCounters();
	InitArithmExpr();
	InitRungs();
	InitSections( );
#ifdef SEQUENTIAL_SUPPORT
	InitSequential( );
#endif
	InitSymbols( );
#if defined( GTK_INTERFACE ) && !defined( MODULE)
	if ( !NoScreenRefresh )
	{
		UpdateVScrollBar( );
		ManagerDisplaySections( );
		DisplaySymbols( );
	}
#endif
}
