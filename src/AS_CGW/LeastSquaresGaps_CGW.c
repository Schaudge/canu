
/**************************************************************************
 * This file is part of Celera Assembler, a software program that 
 * assembles whole-genome shotgun reads into contigs and scaffolds.
 * Copyright (C) 1999-2004, Applera Corporation. All rights reserved.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received (LICENSE.txt) a copy of the GNU General Public 
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *************************************************************************/
static char CM_ID[] = "$Id: LeastSquaresGaps_CGW.c,v 1.4 2005-03-22 19:48:36 jason_miller Exp $";

#define FIXED_RECOMPUTE_SINGULAR /* long standing bug: is it fixed yet? */
#undef LIVE_ON_THE_EDGE	 /* abort on singularities -- this would be a good idea, unless you
			    can't afford to have the assembly crash on a rare problem */

#define FIXED_RECOMPUTE_NOT_ENOUGH_CLONES /* long standing bug: is it fixed yet? it seems to be */

#undef NEG_GAP_VARIANCE_PROBLEM_FIXED  /* if undef'ed, allow processing to continue despite a negative gap variance */


#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/types.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <float.h>

#include "AS_global.h"
#include "AS_UTL_Var.h"
#include "AS_UTL_interval.h"
#include "AS_UTL_timer.h"
#include "AS_CGW_dataTypes.h"
#include "Globals_CGW.h"
#include "DiagnosticsCGW.h"
#include "ScaffoldGraph_CGW.h"
#include "ScaffoldGraphIterator_CGW.h"
#include "ChiSquareTest_CGW.h"
#include "ChunkOverlap_CGW.h"

/* declarations for LAPACK/DXML calls to linear algebra routines */
#define FTN_INT   long int
#define F_FTN_INT    "%ld"


#define MAX_ABSOLUTE_SLOP 10000
#define MAX_SIGMA_SLOP 10000 /* essentially infinite ... */

extern int dgemv_(char *, FTN_INT *, FTN_INT *, 
		  double *, double *, FTN_INT *, double *, FTN_INT *, 
		  double *, double *, FTN_INT *);
extern int dpbtrf_(char *, FTN_INT *, FTN_INT *, double *,
		   FTN_INT *, FTN_INT *);
extern int dpbtrs_(char *, FTN_INT *, FTN_INT *, FTN_INT *, double *,
		   FTN_INT *, double *, FTN_INT *, FTN_INT *);

// Check and fix some wierdnesses that we think creep into LS computations
void   CheckLSScaffoldWierdnesses(char *string,
                                  ScaffoldGraphT *graph,
                                  CIScaffoldT *scaffold);

/// New test code to partly substitute for the status given by MarkInternalEdgeStatus,
/// for to help handle slightly messier cases!
int IsInternalEdgeStatusVaguelyOK(EdgeCGW_T *edge,CDS_CID_t thisCIid);


#define MAX_OVERLAP_SLOP_CGW 10

/* FixupMisorderedContigs
   If we find a pair of contigs that are misordered, we rip out thisCI, move it to the right place based on the implied
   position in the overlapEdge
*/
void FixUpMisorderedContigs(CIScaffoldT *scaffold,
                            ContigT *prevCI, ContigT *thisCI, 
			    ChunkOrientationType edgeOrient, 
			    double inferredMean, double inferredVariance,
			    EdgeCGW_T *overlapEdge){
  ChunkOrientationType newEdgeOrient = GetEdgeOrientationWRT(overlapEdge,
                                                             prevCI->id);
  LengthT aEndOffset, bEndOffset;

  // 1 == prevCI
  // 2 == thisCI
  // edgeOrient is expected orientation of (1,2)
  // newEdgeOrient is correct orientation of (1,2) 

  DumpContig(stderr,ScaffoldGraph, prevCI, FALSE);
  DumpContig(stderr,ScaffoldGraph, thisCI, FALSE);
  PrintGraphEdge(stderr, ScaffoldGraph->ContigGraph, " overlapEdge: ",
                 overlapEdge, overlapEdge->idA);
  fprintf(stderr,"* edgeOrient %c   edge->orient = %c  newEdgeOrient = %c  prevCI = " F_CID "   thisCI = " F_CID " mean:%g\n",
	  edgeOrient, overlapEdge->orient, newEdgeOrient,
          prevCI->id, thisCI->id, inferredMean);
  
  fflush(stderr);

  aEndOffset.mean = aEndOffset.variance = -1.0;
  bEndOffset.mean = bEndOffset.variance = -1.0;
  switch(edgeOrient){

  case AB_AB:
    assert(newEdgeOrient == BA_BA);
    //           expected                                 actual
    //       ---------1-------->                            ---------1-------->
    //                  ---------2-------->     ---------2-------->    
    //                                                      |<=====|
    //                                                      overlap length

    // overlap is negative
    bEndOffset.mean = prevCI->offsetAEnd.mean -  overlapEdge->distance.mean; 
    bEndOffset.variance = prevCI->offsetAEnd.variance +  overlapEdge->distance.variance;
    aEndOffset.mean = bEndOffset.mean - thisCI->bpLength.mean;
    aEndOffset.variance = bEndOffset.variance - thisCI->bpLength.variance;
    break;
  case AB_BA:
    assert(newEdgeOrient == BA_AB);
    //           expected                                 actual
    //       ---------1-------->                            ---------1-------->
    //                  <--------2--------     <--------2--------    
    //                                                      |<=====|
    //                                                      overlap length


    // overlap is negative
    aEndOffset.mean = prevCI->offsetAEnd.mean -  overlapEdge->distance.mean; 
    aEndOffset.variance = prevCI->offsetAEnd.variance + overlapEdge->distance.variance;
    bEndOffset.mean = aEndOffset.mean - thisCI->bpLength.mean;
    bEndOffset.variance = aEndOffset.variance - thisCI->bpLength.variance;
    break;
  case BA_AB:
    assert(newEdgeOrient == AB_BA);
    //           expected                                    actual
    //       <---------1--------                            <---------1--------
    //                  --------2-------->     --------2------->    
    //                                                      |<=====|
    //                                                      overlap length

    // overlap is negative!
    bEndOffset.mean = prevCI->offsetBEnd.mean -  overlapEdge->distance.mean; 
    bEndOffset.variance = prevCI->offsetBEnd.variance -  overlapEdge->distance.variance;
    aEndOffset.mean = bEndOffset.mean - thisCI->bpLength.mean;
    aEndOffset.variance = bEndOffset.variance - thisCI->bpLength.variance;
    break;
  case BA_BA:
    assert(newEdgeOrient == AB_AB);
    //           expected                                 actual
    //       <---------1--------                            <---------1--------
    //                  <---------2--------     <---------2--------    
    //                                                      |<=====|
    //                                                      overlap length

    // overlap is negative
    aEndOffset.mean = prevCI->offsetBEnd.mean - overlapEdge->distance.mean; 
    aEndOffset.variance = prevCI->offsetBEnd.variance +  overlapEdge->distance.variance;
    bEndOffset.mean = aEndOffset.mean - thisCI->bpLength.mean;
    bEndOffset.variance = aEndOffset.variance - thisCI->bpLength.variance;
    break;
  default:
    assert(0);
    break;
  }

  fprintf(stderr,"* Overlap is (" F_CID "," F_CID ",%c)  moving " F_CID " from (%g,%g) to (%g,%g)\n",
	  overlapEdge->idA, overlapEdge->idB, overlapEdge->orient,
	  thisCI->id, thisCI->offsetAEnd.mean, thisCI->offsetBEnd.mean,
	  aEndOffset.mean, bEndOffset.mean);
  thisCI->offsetAEnd = aEndOffset;
  thisCI->offsetBEnd = bEndOffset;
  RemoveCIFromScaffold(ScaffoldGraph, scaffold, thisCI, FALSE);
  InsertCIInScaffold(ScaffoldGraph,
                     thisCI->id, scaffold->id, aEndOffset, bEndOffset,
                     TRUE, FALSE);
}




EdgeCGW_T *FindOverlapEdgeChiSquare(ScaffoldGraphT *graph,
                                    NodeCGW_T *sourceCI,
                                    CDS_CID_t targetId,
                                    ChunkOrientationType edgeOrient,
                                    double inferredMean,
                                    double inferredVariance,
                                    float *chiSquaredValue,
                                    float chiSquareThreshold,
                                    int *alternate, int verbose){
  GraphEdgeIterator edges;
  EdgeCGW_T *edge;
  EdgeCGW_T *bestEdge = (EdgeCGW_T *)NULL;
  int end;
  float bestChiSquaredValue = FLT_MAX;

  *alternate = FALSE;
  if((edgeOrient == AB_AB) || (edgeOrient == AB_BA)){
    /* edgeOrient == AB_XX */
    end = B_END;
  }else{
    /* edgeOrient == BA_XX */
    end = A_END;
  }
  InitGraphEdgeIterator(ScaffoldGraph->RezGraph, sourceCI->id, end, ALL_EDGES,
			GRAPH_EDGE_RAW_ONLY, &edges);// Use raw edges
  while((edge = NextGraphEdgeIterator(&edges))!= NULL){
    CDS_CID_t otherCID = (edge->idA == sourceCI->id) ? edge->idB : edge->idA;
    if((otherCID == targetId) && isOverlapEdge(edge) &&
       !isContainmentEdge(edge) ){// deal with these later
      if(GetEdgeOrientationWRT(edge, sourceCI->id) == edgeOrient){
	if(PairwiseChiSquare((float)inferredMean, (float)inferredVariance,
                             (float)edge->distance.mean,
                             (float)((MAX_OVERLAP_SLOP_CGW * MAX_OVERLAP_SLOP_CGW) / 9),
                             (LengthT *)NULL, chiSquaredValue,
                             (float)chiSquareThreshold)){
	  if(bestEdge == (EdgeCGW_T *)NULL ||
	     (*chiSquaredValue < bestChiSquaredValue)){
	    bestEdge = edge;
	    bestChiSquaredValue = *chiSquaredValue;
	  }
	}
      }
    }
  }

  if(bestEdge != (EdgeCGW_T *)NULL){
    return(bestEdge);
  }
  
  {
    ChunkOverlapCheckT olap;
    CDS_COORD_t minOverlap, maxOverlap;
    minOverlap = max(CGW_MISSED_OVERLAP,
		     -(inferredMean + (3.0 * sqrt(inferredVariance))));
    maxOverlap = -(inferredMean - (3.0 * sqrt(inferredVariance)));
    if(maxOverlap >= CGW_MISSED_OVERLAP){
      float effectiveOlap;
      olap = OverlapChunks(graph->RezGraph,
                           sourceCI->id, targetId,    // handles suspicious
			   edgeOrient, minOverlap, maxOverlap,
                           CGW_DP_ERATE, FALSE);
      effectiveOlap = -olap.overlap;
      if(olap.suspicious){
        fprintf(stderr,"* FOEXS: SUSPICIOUS Overlap found! Looked for (" F_CID "," F_CID ",%c)[" F_COORD "," F_COORD "] found (" F_CID "," F_CID ",%c) " F_COORD "\n",
                sourceCI->id, targetId, edgeOrient,
                minOverlap, maxOverlap,
                olap.spec.cidA, olap.spec.cidB,
                olap.spec.orientation, olap.overlap);
        effectiveOlap = -(GetGraphNode(ScaffoldGraph->ContigGraph, targetId)->bpLength.mean +
                          sourceCI->bpLength.mean - olap.overlap);
      }
      if(olap.overlap){
	CDS_CID_t edgeIndex;
	edgeIndex = InsertComputedOverlapEdge(graph->RezGraph, &olap);
	edge = GetGraphEdge(graph->RezGraph, edgeIndex);
        
	// Create an appropriate hash table entry
	CreateChunkOverlapFromEdge(ScaffoldGraph->RezGraph, edge, FALSE);
        
	if(PairwiseChiSquare((float)inferredMean, (float)inferredVariance,
                             effectiveOlap,
                             (float)((MAX_OVERLAP_SLOP_CGW * MAX_OVERLAP_SLOP_CGW) / 9),
                             (LengthT *)NULL, chiSquaredValue,
                             (float)chiSquareThreshold)){
	  *alternate = olap.suspicious;
	  return(edge);
	}else{
	  fprintf(stderr,"* Failed pairwise test between (%g, %g) and (%g,%g) not returning edge (" F_CID "," F_CID ",%c) %g\n",
		  inferredMean, inferredVariance, effectiveOlap, (float) ((MAX_OVERLAP_SLOP_CGW * MAX_OVERLAP_SLOP_CGW) / 9),
		  edge->idA, edge->idB, edge->orient, edge->distance.mean);
	}
      }
    }
    return((EdgeCGW_T *)NULL);
  }
}

void CheckInternalEdgeStatus(ScaffoldGraphT *graph, CIScaffoldT *scaffold, 
                             float pairwiseChiSquaredThreshhold,
                             float maxVariance,
                             int doNotChange, int verbose){
  CIScaffoldTIterator CIs;
  /* Iterate over all of the CIEdges */
  GraphEdgeIterator edges;
  EdgeCGW_T *edge;
  NodeCGW_T *thisCI;
  int32 numCIs;
  int32 indexCIs;
  
  fprintf(GlobalData->logfp, "Checking Edges for Scaffold " F_CID "\n",
	  scaffold->id);
  
  InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
  
  for(indexCIs = 0;
      (thisCI = NextCIScaffoldTIterator(&CIs)) != NULL;){
    thisCI->indexInScaffold = indexCIs;
    indexCIs++;
  }
  numCIs = indexCIs;
  if(numCIs != scaffold->info.Scaffold.numElements){
    fprintf(GlobalData->logfp, "NumElements inconsistent %d,%d\n",
	    numCIs, scaffold->info.Scaffold.numElements);
    scaffold->info.Scaffold.numElements = numCIs;
  }
  
  assert(indexCIs == numCIs);
  
  InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
  
  while((thisCI = NextCIScaffoldTIterator(&CIs)) != NULL){
    
    // Use merged edges
    InitGraphEdgeIterator(ScaffoldGraph->RezGraph, thisCI->id, ALL_END,
                          ALL_EDGES, GRAPH_EDGE_DEFAULT, &edges);

    while((edge = NextGraphEdgeIterator(&edges))!= NULL){
      int isA = (edge->idA == thisCI->id);
      NodeCGW_T *otherCI =
	GetGraphNode(ScaffoldGraph->RezGraph,
                     (isA? edge->idB: edge->idA));
      ChunkOrientationType edgeOrient;
      FragOrient thisCIorient, otherCIorient;
      LengthT gapDistance;
      float chiSquareResult;
      
      /* We do not want to check edges with certain
	 labels as specified in the doNotChange mask. */
      if(edge->flags.bits.edgeStatus & doNotChange){
	continue;
      }
      /* Only edges between CIs in the same scaffold should be trusted. */
      if(otherCI->scaffoldID != thisCI->scaffoldID){
	if(verbose){
	  EdgeStatus edgeStatus = GetEdgeStatus(edge);
	  if((edgeStatus == TRUSTED_EDGE_STATUS) ||
	     (edgeStatus == TENTATIVE_TRUSTED_EDGE_STATUS)){
	    fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Trusted edge really interscaffold edge.\n",
		    thisCI->id, thisCI->scaffoldID, otherCI->id,
		    otherCI->scaffoldID);
	  }else if(edgeStatus != INTER_SCAFFOLD_EDGE_STATUS){
	    fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Interscaffold edge marked as %d.\n",
		    thisCI->id, thisCI->scaffoldID, otherCI->id,
		    otherCI->scaffoldID, edgeStatus);
	  }
	}
	continue;
      }
      /* We only want to check an edge once and the
	 iterator will visit intra scaffold edges twice so we use
	 the condition that the edge is canonical - that is the
	 condition thisCI->info.CI.indexInScaffold < otherCI->info.CI.indexInScaffold
	 to guarantee labelling the edge only once. */
      if(otherCI->indexInScaffold <= thisCI->indexInScaffold){
	continue;
      }
      
      /* Check that the edge orientation is consistent with the CI positions
	 and orientations within the scaffold. */
      edgeOrient = GetEdgeOrientationWRT(edge, thisCI->id);
      switch(edgeOrient){
        case AB_BA:
          //      thisCI                                        otherCI
          //  A --------------------- B               B --------------------- A
          //    5'----->                                           <------5'
          thisCIorient = A_B;
          otherCIorient = B_A;
          gapDistance.mean = otherCI->offsetBEnd.mean - thisCI->offsetBEnd.mean;
          gapDistance.variance = otherCI->offsetBEnd.variance -
            thisCI->offsetBEnd.variance;
          break;
        case AB_AB:
          //      thisCI                                        otherCI
          //  A --------------------- B               A --------------------- B
          //    5'----->                                           <------5'
          thisCIorient = A_B;
          otherCIorient = A_B;
          gapDistance.mean = otherCI->offsetAEnd.mean - thisCI->offsetBEnd.mean;
          gapDistance.variance = otherCI->offsetAEnd.variance -
            thisCI->offsetBEnd.variance;
          break;
        case BA_BA:
          //      thisCI                                        otherCI
          //  B --------------------- A               B --------------------- A
          //    5'----->                                           <------5'
          thisCIorient = B_A;
          otherCIorient = B_A;
          gapDistance.mean = otherCI->offsetBEnd.mean - thisCI->offsetAEnd.mean;
          gapDistance.variance = otherCI->offsetBEnd.variance -
            thisCI->offsetAEnd.variance;
          break;
        case BA_AB:
          //      thisCI                                        otherCI
          //  B --------------------- A               A --------------------- B
          //    5'----->                                           <------5'
          thisCIorient = B_A;
          otherCIorient = A_B;
          gapDistance.mean = otherCI->offsetAEnd.mean - thisCI->offsetAEnd.mean;
          gapDistance.variance = otherCI->offsetAEnd.variance -
            thisCI->offsetAEnd.variance;
          break;
        default:
          assert(0);
          break;
      }
      if((GetNodeOrient(thisCI) != thisCIorient) ||
	 (GetNodeOrient(otherCI) != otherCIorient)){
	/* Mark as untrusted an edge whose orientation does not agree
	   with the orientation of the CIs in the scaffold. */
	EdgeStatus edgeStatus = GetEdgeStatus(edge);
	if((edgeStatus == TRUSTED_EDGE_STATUS) ||
	   (edgeStatus == TENTATIVE_TRUSTED_EDGE_STATUS)){
	  fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Trusted edge really Bad orientation (%c,%c) (%c,%c).\n",
		  thisCI->id, thisCI->scaffoldID, otherCI->id,
		  otherCI->scaffoldID, GetNodeOrient(thisCI), thisCIorient,
		  GetNodeOrient(otherCI), otherCIorient);
	}else if((edgeStatus != UNTRUSTED_EDGE_STATUS) &&
		 (edgeStatus != TENTATIVE_UNTRUSTED_EDGE_STATUS)){
	  fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Bad orientation (%c,%c) (%c,%c) edge marked as %d.\n",
		  thisCI->id, thisCI->scaffoldID, otherCI->id,
		  otherCI->scaffoldID, GetNodeOrient(thisCI), thisCIorient,
		  GetNodeOrient(otherCI), otherCIorient, edgeStatus);
	}
	continue;
      }
      if(gapDistance.variance <= 0.0){
	EdgeStatus edgeStatus = GetEdgeStatus(edge);
	fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Bad Gap Variance (%f,%f) (%f,%f) edge marked as %d.\n",
		thisCI->id, thisCI->scaffoldID, otherCI->id,
		otherCI->scaffoldID,
		gapDistance.mean, gapDistance.variance,
		edge->distance.mean, edge->distance.variance, edgeStatus);
	DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,scaffold,TRUE);
	DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,scaffold,FALSE);

      }else if(!PairwiseChiSquare((float)gapDistance.mean,
				  gapDistance.variance,
				  (float)edge->distance.mean,
				  edge->distance.variance,
				  (LengthT *)NULL, &chiSquareResult,
				  pairwiseChiSquaredThreshhold)){
	/* Mark  this edge as untrusted if the distance of the edge is not
	   consistent with the estimated gap distance as judged by the
	   Chi Squared Test. */
	EdgeStatus edgeStatus = GetEdgeStatus(edge);
	if((edgeStatus == TRUSTED_EDGE_STATUS) ||
	   (edgeStatus == TENTATIVE_TRUSTED_EDGE_STATUS)){
	  fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Trusted edge really Bad Chi Squared %f (%f,%f) (%f,%f).\n",
		  thisCI->id, thisCI->scaffoldID, otherCI->id,
		  otherCI->scaffoldID,
		  chiSquareResult, gapDistance.mean, gapDistance.variance,
		  edge->distance.mean, edge->distance.variance);
	}else if((edgeStatus != UNTRUSTED_EDGE_STATUS) &&
		 (edgeStatus != TENTATIVE_UNTRUSTED_EDGE_STATUS)){
	  fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Bad Chi Squared %f (%f,%f) (%f,%f) edge marked as %d.\n",
		  thisCI->id, thisCI->scaffoldID, otherCI->id,
		  otherCI->scaffoldID,
		  chiSquareResult, gapDistance.mean, gapDistance.variance,
		  edge->distance.mean, edge->distance.variance, edgeStatus);
	}
	continue;
      }
      if(edge->distance.variance > maxVariance){
	EdgeStatus edgeStatus = GetEdgeStatus(edge);
	if((edgeStatus == TRUSTED_EDGE_STATUS) ||
	   (edgeStatus == TENTATIVE_TRUSTED_EDGE_STATUS)){
	  fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Trusted edge really Variance too large %f.\n",
		  thisCI->id, thisCI->scaffoldID, otherCI->id,
		  otherCI->scaffoldID, edge->distance.variance);
	}else if(edgeStatus != LARGE_VARIANCE_EDGE_STATUS){
	  fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Variance too large %f edge marked as %d.\n",
		  thisCI->id, thisCI->scaffoldID, otherCI->id,
		  otherCI->scaffoldID, edge->distance.variance, edgeStatus);
	}
	continue;
      }
      {
	EdgeStatus edgeStatus = GetEdgeStatus(edge);
	if((edgeStatus != TRUSTED_EDGE_STATUS) &&
	   (edgeStatus != TENTATIVE_TRUSTED_EDGE_STATUS)){
	  if(verbose){
	    fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Edge marked as %d should be trusted.\n",
		    thisCI->id, thisCI->scaffoldID, otherCI->id,
		    otherCI->scaffoldID, edgeStatus);
	    fprintf(GlobalData->logfp, " - Good Chi Squared %f (%f,%f) (%f,%f)\n",
		    chiSquareResult, gapDistance.mean, gapDistance.variance,
		    edge->distance.mean, edge->distance.variance);
	  }
	}
      }
    }
  }
  return;
}

typedef struct {
  int numCIs;
  int numClones;
  int numGaps;
  size_t sizeofLengthCIs;
  size_t sizeofCloneGapStart;
  size_t sizeofCloneGapEnd;
  size_t sizeofGapConstants;
  size_t sizeofGapCoefficients;
  size_t sizeofGapVariance;
  size_t sizeofCloneVariance;
  size_t sizeofCloneMean;
  size_t sizeofSpannedGaps;
  size_t sizeofGapSize;
  size_t sizeofGapSizeVariance;
  size_t sizeofGapsToComputeGaps;
  size_t sizeofComputeGapsToGaps;
  LengthT *lengthCIs;
  CDS_COORD_t *cloneGapStart;
  CDS_COORD_t *cloneGapEnd;
  double *gapConstants;
  double *gapCoefficients;
  double *gapVariance;
  double *cloneVariance;
  double *cloneMean;
  double *spannedGaps;
  double *gapSize;
  double *gapSizeVariance;
  int *gapsToComputeGaps;
  int *computeGapsToGaps;
} RecomputeData;

void freeRecomputeData(RecomputeData *data){
  free(data->lengthCIs);
  free(data->gapConstants);
  free(data->gapCoefficients);
  free(data->gapVariance);
  free(data->cloneGapStart);
  free(data->cloneGapEnd);
  free(data->cloneVariance);
  free(data->cloneMean);
  free(data->spannedGaps);
  free(data->gapSize);
  free(data->gapSizeVariance);
  free(data->gapsToComputeGaps);
  free(data->computeGapsToGaps);
}
void ReportRecomputeData(RecomputeData *data, FILE *stream){
  size_t totalMemorySize = 0;
  totalMemorySize = 
    data->sizeofLengthCIs +
    data->sizeofCloneGapStart +
    data->sizeofCloneGapEnd +
    data->sizeofGapConstants +
    data->sizeofGapCoefficients +
    data->sizeofGapVariance +
    data->sizeofCloneVariance +
    data->sizeofCloneMean +
    data->sizeofSpannedGaps +
    data->sizeofGapSize +
    data->sizeofGapSizeVariance +
    data->sizeofGapsToComputeGaps +
    data->sizeofComputeGapsToGaps;

  if(totalMemorySize > 1<<30) // if > 1GB
    fprintf(stream, "* Recompute Offsets CIs:%d Clones:%d Gaps:%d allocated " F_SIZE_T " bytes\n",
            data->numCIs,
            data->numClones,
            data->numGaps,
            totalMemorySize);
}

RecomputeOffsetsStatus RecomputeOffsetsInScaffold(ScaffoldGraphT *graph,
                                                  CIScaffoldT *scaffold,
                                                  int allowOrderChanges,
                                                  int forceNonOverlaps,
                                                  int verbose){

  RecomputeData data;
  CIScaffoldTIterator CIs;
  /* Iterate over all of the "trusted" CIEdges */
  GraphEdgeIterator edges;
  EdgeCGW_T *edge;
  NodeCGW_T *thisCI, *prevCI;
  int32 numCIs;
  int32 indexCIs;
  int standardEdgeStatusFails =0;

#undef DEBUG_LS
#ifdef DEBUG_LS
  verbose=1;
#endif

  int numGaps, numComputeGaps;
  LengthT *lengthCIs, *lengthCIsPtr;
  int maxDiagonals = 1;
  int numClones = 0;
  CDS_CID_t indexClones;
  CDS_COORD_t *cloneGapStart, *cloneGapEnd;
  int *gapsToComputeGaps, *computeGapsToGaps;
  double *gapCoefficients, *gapConstants;
  double *gapVariance, *cloneVariance;
  double *cloneMean;
  double *spannedGaps;
  double *gapSize, *gapSizeVariance;
  double squaredError;
  LengthT *maxOffset = NULL;
  int hardConstraintSet;

  data.lengthCIs = NULL;
  data.cloneGapStart = NULL;
  data.cloneGapEnd = NULL;
  data.gapConstants = NULL;
  data.gapCoefficients = NULL;
  data.gapVariance = NULL;
  data.cloneVariance = NULL;
  data.cloneMean = NULL;
  data.spannedGaps = NULL;
  data.gapSize = NULL;
  data.gapSizeVariance = NULL;
  data.gapsToComputeGaps = NULL;
  data.computeGapsToGaps = NULL;

  StartTimerT(&GlobalData->RecomputeOffsetsTimer);   /*  START */



#if 1
  //  DumpCIScaffold(graph, scaffold, FALSE);
  CheckInternalEdgeStatus(graph, scaffold, PAIRWISECHI2THRESHOLD_CGW,
                          100000000000.0, 0, FALSE);
#endif
  fprintf(GlobalData->logfp, "Recompute Offsets for Scaffold " F_CID "\n",
	  scaffold->id);
  // DumpCIScaffold(graph, scaffold, FALSE);

  if(IsScaffoldInternallyConnected(ScaffoldGraph,scaffold,ALL_TRUSTED_EDGES)!=1){
    standardEdgeStatusFails =1;
    fprintf(stderr,"WARNING: RecomputeOffsetsInScaffold(): scaffold " F_CID " is not internally connected using ALL_TRUSTED_EDGES -- will proceed with edge set determined by IsInternalEdgeStatusVaguelyOK instead of PairwiseChiSquare test\n",scaffold->id);
  } else {
    /*    fprintf(stderr,"NOTICE: RecomputeOffsetsInScaffold(): scaffold " F_CID " is internally connected using ALL_TRUSTED_EDGES -- will proceed as normal\n",
	  scaffold->id); */
  }

  numCIs = scaffold->info.Scaffold.numElements;
  numGaps = numCIs - 1;
  if(numGaps < 1){
    freeRecomputeData(&data);
    return (RECOMPUTE_NO_GAPS);
  }
  data.numCIs = numCIs;
  data.sizeofLengthCIs = numCIs * sizeof(*lengthCIs);
  data.lengthCIs = lengthCIs = (LengthT *)malloc(data.sizeofLengthCIs);
  AssertPtr(lengthCIs);
  
  InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
  
  for(indexCIs = 0, lengthCIsPtr = lengthCIs;
      (thisCI = NextCIScaffoldTIterator(&CIs)) != NULL;){
    thisCI->indexInScaffold = indexCIs;
    *lengthCIsPtr = thisCI->bpLength;
    if(verbose)
      fprintf(GlobalData->logfp, "Length of CI %d," F_CID " %f\n",
	      indexCIs, thisCI->id, lengthCIsPtr->mean);

    if(verbose)
      fprintf(stderr, "Length of CI %d," F_CID " %f\n",
	      indexCIs, thisCI->id, lengthCIsPtr->mean);

    indexCIs++;
    lengthCIsPtr++;
  }
  assert(indexCIs == numCIs);
  
  InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
  
  while((thisCI = NextCIScaffoldTIterator(&CIs)) != NULL){

    InitGraphEdgeIterator(ScaffoldGraph->RezGraph, thisCI->id, 
			  ALL_END,
			  (standardEdgeStatusFails ? ALL_INTERNAL_EDGES : ALL_TRUSTED_EDGES),
			  GRAPH_EDGE_RAW_ONLY,
//			  GRAPH_EDGE_RAW_ONLY | (standardEdgeStatusFails ? GRAPH_EDGE_VERBOSE : 0),
			  &edges);// ONLY RAW
    
    while((edge = NextGraphEdgeIterator(&edges))!= NULL){
      int isA = (edge->idA == thisCI->id);
      NodeCGW_T *otherCI =
	GetGraphNode(ScaffoldGraph->RezGraph,
                     (isA? edge->idB: edge->idA));

      // RAW EDGES ONLY
      assert(edge->flags.bits.isRaw);
      
      if(otherCI->indexInScaffold <= thisCI->indexInScaffold){
	continue; // Only interested in looking at an edge once
      }


      // the following is invoked if we are looking at ALL_EDGES rather than ALL_TRUSTED_EDGES;
      // this occurs as a desperate attempt to resurrect some scaffold edges that will reconnect 
      // a scaffold that is otherwise disconnected and results in a singularity; however, if
      // this edge is really really bad, we need to throw it away ...
      if(standardEdgeStatusFails && !IsInternalEdgeStatusVaguelyOK(edge,thisCI->id)){
	continue;
      }


      numClones++;
      if(verbose)
	fprintf(stderr,"RecomputeOffsets: adding clone between %d and %d\n",
		thisCI->id,otherCI->id);

      if((otherCI->indexInScaffold - thisCI->indexInScaffold) >
	 maxDiagonals){
	maxDiagonals = otherCI->indexInScaffold - thisCI->indexInScaffold;
	if(verbose){
	  fprintf(GlobalData->logfp, "Max Diagonals %d (%d,%d) [" F_CID "." F_CID "," F_CID "." F_CID "]\n",
		  maxDiagonals, thisCI->indexInScaffold,
		  otherCI->indexInScaffold, thisCI->scaffoldID,
		  thisCI->id, otherCI->scaffoldID, otherCI->id);
	  fprintf(stderr, "Max Diagonals %d (%d,%d) [" F_CID "." F_CID "," F_CID "." F_CID "]\n",
		  maxDiagonals, thisCI->indexInScaffold,
		  otherCI->indexInScaffold, thisCI->scaffoldID,
		  thisCI->id, otherCI->scaffoldID, otherCI->id);
	}
      }
    }
  }
  if(numClones < numGaps){
    freeRecomputeData(&data);
#ifdef  FIXED_RECOMPUTE_NOT_ENOUGH_CLONES
    assert(0 /* Not enough clones */);
#endif
    return (RECOMPUTE_NOT_ENOUGH_CLONES);
  }
  {
    double *gapEnd, *gapPtr;
    
    data.sizeofCloneGapStart = numClones * sizeof(*cloneGapStart);
    data.cloneGapStart =
      cloneGapStart = (int32 *)malloc(data.sizeofCloneGapStart);
    AssertPtr(cloneGapStart);
    data.sizeofCloneGapEnd = numClones * sizeof(*cloneGapEnd);
    data.cloneGapEnd =
      cloneGapEnd = (int32 *)malloc(data.sizeofCloneGapEnd);
    AssertPtr(cloneGapEnd);
    data.sizeofGapConstants = numGaps * sizeof(*gapConstants);
    data.gapConstants =
      gapConstants = (double *)malloc(data.sizeofGapConstants);
    AssertPtr(gapConstants);
    for(gapPtr = gapConstants, gapEnd = gapPtr + numGaps;
	gapPtr < gapEnd; gapPtr++){
      *gapPtr = 0.0;
    }
    data.sizeofGapCoefficients = (maxDiagonals * numGaps) *
      sizeof(*gapCoefficients);
    data.gapCoefficients =
      gapCoefficients = (double *)malloc(data.sizeofGapCoefficients);
    AssertPtr(gapCoefficients);
    for(gapPtr = gapCoefficients, gapEnd = gapPtr + (maxDiagonals * numGaps);
	gapPtr < gapEnd; gapPtr++){
      *gapPtr = 0.0;
    }
    data.numClones = numClones;
    data.numGaps = numGaps;
    data.sizeofCloneMean = numClones * sizeof(*cloneMean);
    data.cloneMean = cloneMean = (double *)malloc(data.sizeofCloneMean);
    AssertPtr(cloneMean);
    data.sizeofCloneVariance = numClones * sizeof(*cloneVariance);
    data.cloneVariance =
      cloneVariance = (double *)malloc(data.sizeofCloneVariance);
    AssertPtr(cloneVariance);
    data.sizeofGapVariance = numGaps * sizeof(*gapVariance);
    data.gapVariance = gapVariance = (double *)malloc(data.sizeofGapVariance);
    AssertPtr(gapVariance);
    for(gapPtr = gapVariance, gapEnd = gapPtr + numGaps;
	gapPtr < gapEnd; gapPtr++){
      *gapPtr = 0.0;
    }
    data.sizeofSpannedGaps = numGaps * sizeof(*spannedGaps);
    data.spannedGaps = spannedGaps = (double *)malloc(data.sizeofSpannedGaps);
    AssertPtr(spannedGaps);
    data.sizeofGapSize = numGaps * sizeof(*gapSize);
    data.gapSize = gapSize = (double *)malloc(data.sizeofGapSize);
    AssertPtr(gapSize);
    data.sizeofGapSizeVariance = numGaps * sizeof(*gapSizeVariance);
    data.gapSizeVariance =
      gapSizeVariance = (double *)malloc(data.sizeofGapSizeVariance);
    AssertPtr(gapSizeVariance);
    data.sizeofGapsToComputeGaps = numGaps * sizeof(*gapsToComputeGaps);
    data.gapsToComputeGaps =
      gapsToComputeGaps = (int32 *)malloc(data.sizeofGapsToComputeGaps);
    AssertPtr(gapsToComputeGaps);
    data.sizeofComputeGapsToGaps = numGaps * sizeof(*computeGapsToGaps);
    data.computeGapsToGaps =
      computeGapsToGaps = (int32 *)malloc(data.sizeofComputeGapsToGaps);
    AssertPtr(computeGapsToGaps);
    for(numComputeGaps = 0; numComputeGaps < numGaps; numComputeGaps++){
      gapsToComputeGaps[numComputeGaps] =
	computeGapsToGaps[numComputeGaps] = numComputeGaps;
    }
    ReportRecomputeData(&data, stderr);
  }
  /* The following code solves a set of linear equations in order to
     find a least squares minimal solution for the length of the gaps
     between CIs within this scaffold. The squared error to be
     minimized is defined to be the expected size of the gaps spanned
     by a clone minus the gap sizes we are solving for spanned by the
     clone squared divided by the variance of the expected size summed
     over all clones. The expected size of the gaps spanned by a clone
     is computed as follows: the expected/mean length of the clone is
     provided as an input parameter based on what DNA library the clone
     is from, from this mean length we then subtract the portions of
     the CIs which contain the clone end fragments (this step has already
     been done for us and is encoded in the edge->distance record as
     the mean - the variance is also previously computed based on the
     assumption that the two random variables are independent so that
     the variances are additive), in addition we subtract the lengths of
     CIs entirely spanned by the clone (this depends on knowing the order
     of the CIs which is provided by the scaffold) and again add the
     variances assuming independence. In order to find the least squares
     minimal solution we take the partial derivatives of the squared
     error with respect to the gap sizes we are solving for and setting
     them to zero resulting in numGaps equations with numGaps unknowns.
     We use the LAPACK tools to solve this set of equations. Note that
     each term in the squared error sum contributes to a particular
     partial derivative iff the clone for that term spans the gap for
     that partial derivative. */
  do{
    int maxClone;
    indexClones = 0;
    InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
    
    while((thisCI = NextCIScaffoldTIterator(&CIs)) != NULL){
      
      InitGraphEdgeIterator(ScaffoldGraph->RezGraph, thisCI->id, 
			    ALL_END,
			    (standardEdgeStatusFails ? ALL_INTERNAL_EDGES : ALL_TRUSTED_EDGES),
			    GRAPH_EDGE_RAW_ONLY,
//			    GRAPH_EDGE_RAW_ONLY | (standardEdgeStatusFails ? GRAPH_EDGE_VERBOSE : 0),
			    &edges);// ONLY RAW
      
      while((edge = NextGraphEdgeIterator(&edges))!= NULL){
	int isA = (edge->idA == thisCI->id);
	NodeCGW_T *otherCI =
	  GetGraphNode(ScaffoldGraph->RezGraph,
                       (isA? edge->idB: edge->idA));
	double constant, constantVariance, inverseVariance;
	int lengthCIsIndex, gapIndex;
	int colIndex;
        
	// RAW EDGES ONLY
	assert(edge->flags.bits.isRaw);
        
	if(otherCI->indexInScaffold <= thisCI->indexInScaffold){
	  continue; // Only interested in looking at an edge once
	}

	// the following is paired up with a similar test above--see comment there
	if(standardEdgeStatusFails && !IsInternalEdgeStatusVaguelyOK(edge,thisCI->id)){
	  continue;
	}


	if(indexClones>=numClones){
	  
	  fprintf(stderr,"ROIS: Enlarging clone-dependent arrays -- must have improved layout enough to rescue some more clones\n");
	  numClones*=1.2;
	  data.numClones=numClones;

	  data.sizeofCloneMean = numClones * sizeof(*cloneMean);
	  data.cloneMean = cloneMean = (double *)realloc(cloneMean,data.sizeofCloneMean);
	  AssertPtr(cloneMean);
	  data.sizeofCloneVariance = numClones * sizeof(*cloneVariance);
	  data.cloneVariance =
	    cloneVariance = (double *)realloc(cloneVariance,data.sizeofCloneVariance);
	  AssertPtr(cloneVariance);

	  data.sizeofCloneGapStart = numClones * sizeof(*cloneGapStart);
	  data.cloneGapStart =
	    cloneGapStart = (int32 *)realloc(cloneGapStart,data.sizeofCloneGapStart);
	  AssertPtr(cloneGapStart);

	  data.sizeofCloneGapEnd = numClones * sizeof(*cloneGapEnd);
	  data.cloneGapEnd =
	    cloneGapEnd = (int32 *)realloc(cloneGapEnd,data.sizeofCloneGapEnd);
	  AssertPtr(cloneGapEnd);

	}


	/* We compute the mean and variance for the estimated total gap size
	   for this particular clone. We start with the edge mean and variance
	   which already takes into account the clone mean and variance and
	   the portions of the CIs containing the clone ends mean and variance.
	   Next we subtract the length of all of the CIs spanned by the clone.
	   Again we assume the variances are additive based on independence. */
	for(constant = edge->distance.mean,
	      constantVariance = edge->distance.variance,
	      lengthCIsIndex = thisCI->indexInScaffold + 1;
	    lengthCIsIndex < otherCI->indexInScaffold; lengthCIsIndex++){
	  constant -= lengthCIs[lengthCIsIndex].mean;
	  constantVariance += lengthCIs[lengthCIsIndex].variance;
	}
	/* If we are recomputing gap sizes after setting some of the gaps to
	   a fixed size based on the lack of an expected overlap then we need
	   to take these fixed gaps and their variances into account -
	   otherwise this loop is a no-op. The question is how to adjust the
	   existing variance if at all. Adding it in produces huge variances
	   which seems wrong but not doing anything seems wrong too. */
	for(gapIndex = thisCI->indexInScaffold;
	    gapIndex < otherCI->indexInScaffold; gapIndex++){
	  if(gapsToComputeGaps[gapIndex] == NULLINDEX){
	    constant -= gapSize[gapIndex];
	    //constantVariance += gapSizeVariance[gapIndex];
	  }
	}
	/* cloneMean and cloneVariance are the statistics for the estimated
	   total size of the gaps spanned by this clone. */
	cloneMean[indexClones] = constant;
	cloneVariance[indexClones] = constantVariance;
	if(verbose){
	  fprintf(GlobalData->logfp, "Gap clone %f,%f (%d,%d)\n",
                  constant, sqrt(constantVariance),
		  thisCI->indexInScaffold, otherCI->indexInScaffold);
	  fflush(GlobalData->logfp);
	}
	constant /= constantVariance;
	inverseVariance = 1.0 / constantVariance;
	/* Store which gaps each clone spans so that we can iterate over
	   these gaps when we calculate the gap variances and the
	   squared error. */
	cloneGapStart[indexClones] = thisCI->indexInScaffold;
	cloneGapEnd[indexClones] = otherCI->indexInScaffold;
	/* Below we incrementally add to the matrices and vector we need for
	   solving our equations. When we take the partial derivatives and
	   set them to zero we get numGaps equations which we can represent
	   as a vector on one side of equation by moving the constant terms
	   to one side and a matrix times our set of gap size variables on
	   the other. The vector is called gapConstants and the matrix
	   gapCoefficients. As expected gapConstants is stored as a one
	   dimensional array. The storage for gapCoefficients is also a
	   one dimensional array but it represents a more complicated
	   data structure. First due to the local effects of the clones
	   on the scaffold the array is usually banded so for efficiency
	   we only store the nonzero bands and in addition the matrix is
	   symmetric so we only store the lower bands (subdiagonals) plus
	   the main diagonal. The LAPACK interface expects the subdiagonals
	   to be padded out to the same length as the diagonal and to be in
	   column major order with the diagonals stored as rows so we
	   comply. */
	for(colIndex = thisCI->indexInScaffold;
	    colIndex < otherCI->indexInScaffold; colIndex++){
          int rowIndex;
	  int colComputeIndex = gapsToComputeGaps[colIndex];
	  /* For each gap that the clone spans it contributes the same
	     constant value to the gapConstants vector which is equal
	     to the mean total gap size for that clone divided by the
	     variance of the total gap size. */
	  if(colComputeIndex == NULLINDEX){
	    continue;
	  }
	  gapConstants[colComputeIndex] += constant;
	  for(rowIndex = colIndex;
	      rowIndex < otherCI->indexInScaffold; rowIndex++){
	    int rowComputeIndex = gapsToComputeGaps[rowIndex];
	    /* If the number of gaps spanned by the clone is N then this clone
	       contributes to NxN terms in the gapCoefficients matrix, but
	       because the matrix is symmetric we only store the lower triangle
	       so N*(N+1)/2 terms are affected for this clone. Remember that we
	       store the (sub)diagonals as rows in column major order because
	       the matrix tends to be banded and to use the LAPACK interface.
	       The contribution of this clone to each term is the inverse of
	       the variance of the total gap size for that clone. */
	    if(rowComputeIndex == NULLINDEX){
	      continue;
	    }
	    gapCoefficients[(colComputeIndex * maxDiagonals)
                            + (rowComputeIndex - colComputeIndex)] += inverseVariance;
	  }
	}
	indexClones++;
      }
    }

    maxClone=indexClones;
    {
      FTN_INT nrhs = 1;
      FTN_INT bands = maxDiagonals - 1;
      FTN_INT ldab = maxDiagonals;
      FTN_INT rows = numComputeGaps;
      FTN_INT info = 0;
      
      if(verbose){
	int i = 0;
	double *gapEnd, *gapPtr;
	for(gapPtr = gapConstants, gapEnd = gapPtr + numComputeGaps;
	    gapPtr < gapEnd; gapPtr++){
	  fprintf(GlobalData->logfp, "Gap Constants %g\n", *gapPtr);
	}
	fprintf(GlobalData->logfp, "Gap Coefficients\n");
	for(gapPtr = gapCoefficients, gapEnd = gapPtr + (maxDiagonals * numComputeGaps);
	    gapPtr < gapEnd; gapPtr++){
	  fprintf(GlobalData->logfp, "%g", *gapPtr);
	  i++;
	  if(i == maxDiagonals){
	    fprintf(GlobalData->logfp, "\n");
	    i = 0;
	  }else{
	    fprintf(GlobalData->logfp, "\t\t");
	  }
	}

	fprintf(stderr, "rows " F_FTN_INT " bands " F_FTN_INT " ldab " F_FTN_INT " info " F_FTN_INT "\n",
		rows, bands, ldab, info);

      }
      
      dpbtrf_("L", &rows, &bands, gapCoefficients, &ldab, &info);
      if(verbose)
	fprintf(GlobalData->logfp, "dpbtrf: rows " F_FTN_INT " bands " F_FTN_INT " ldab " F_FTN_INT " info " F_FTN_INT "\n",
		rows, bands, ldab, info);
      if(info < 0){
	freeRecomputeData(&data);
	assert(0 /* RECOMPUTE_LAPACK */);
	return (RECOMPUTE_LAPACK);
      }else if(info > 0){
	freeRecomputeData(&data);

#ifdef FIXED_RECOMPUTE_SINGULAR        
        // mjf 3/9/2001
        // this assert was causing trouble in the mouse_20010307 run, commented it out
        // and the run proceeded w/o further trouble
        // need to figure out why scaffolds that were apparently connected go singular
	DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,scaffold,TRUE);
	DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,scaffold,FALSE);

	fprintf(stderr,"WARNING : SOMEBODY IS SCREWING UP SCAFFOLDING -- RecomputeOffsetsInScaffold has a singularity\n");
  #ifdef LIVE_ON_THE_EDGE	
        assert(0 /* RECOMPUTE_SINGULAR */);
  #endif
  	
#endif
	return (RECOMPUTE_SINGULAR);
      }
      /* Call an LAPACK routine to multiply the inverse of the gapCoefficients
	 matrix by the gapConstants vector resulting in the least squares
	 minimal solution of the gap sizes being returned in the gapConstants
	 vector. */
      dpbtrs_("L", &rows, &bands, &nrhs, gapCoefficients, &ldab,
	      gapConstants, &rows, &info);
      if(verbose)
	fprintf(GlobalData->logfp, "dpbtrs (call1): rows " F_FTN_INT " bands " F_FTN_INT " ldab " F_FTN_INT " nrhs " F_FTN_INT " info " F_FTN_INT "\n",
		rows, bands, ldab, nrhs, info);
      if(info < 0){
	freeRecomputeData(&data);
	assert(0 /* RECOMPUTE_LAPACK */);
	return (RECOMPUTE_LAPACK);
      }else if(info > 0){
	freeRecomputeData(&data);
	assert(0 /* RECOMPUTE_SINGULAR */);
	return (RECOMPUTE_SINGULAR);
      }
    }
      
    squaredError = 0;
    for(indexClones = 0; indexClones < maxClone; indexClones++){
      int gapIndex;
      int contributesToVariance = FALSE;
      
      /* We compute the squared error and gap size variances incrementally
	 by adding the contribution from each clone. */
      for(gapIndex = 0; gapIndex < numComputeGaps; gapIndex++){
	spannedGaps[gapIndex] = 0.0;
      }
      for(gapIndex = cloneGapStart[indexClones];
	  gapIndex < cloneGapEnd[indexClones]; gapIndex++){
	/* Compute the expected total gap size for this clone minus the solved
	   for gap sizes that this clone spans. */
	if(gapsToComputeGaps[gapIndex] != NULLINDEX){
	  cloneMean[indexClones] -= gapConstants[gapsToComputeGaps[gapIndex]];
	  /* Finish creating a vector whose components are 0.0 for gaps not
	     spanned by this clone and 1.0 for gaps that are. */
	  spannedGaps[gapsToComputeGaps[gapIndex]] = 1.0;
	  contributesToVariance = TRUE;
	}
      }
      /* To compute the squared error we square the difference between
	 the expected total gap size for this clone minus the solved
	 for gap sizes that this clone spans and divide by the clone
	 variance. */
      squaredError += (cloneMean[indexClones] * cloneMean[indexClones]) /
	cloneVariance[indexClones];
      if(contributesToVariance){
	FTN_INT nrhs = 1;
	FTN_INT bands = maxDiagonals - 1;
	FTN_INT ldab = maxDiagonals;
	FTN_INT rows = numComputeGaps;
	FTN_INT info = 0;
	double *gapEnd, *gapPtr, *gapPtr2;
	
	/* Multiply the inverse of the gapCoefficients matrix times the vector
	   of which gaps were spanned by this clone to produce the derivative
	   of the gap sizes with respect to this clone (actually we would need
	   to divide by the total gap variance for this clone
	   but we correct for this below).
	   This is computed in order to get an estimate of the variance for
	   the gap sizes we have determined as outlined in equation 5-7 page
	   70 of Data Reduction and Error Analysis for the Physical Sciences
	   by Philip R. Bevington. */
	dpbtrs_("L", &rows, &bands, &nrhs, gapCoefficients, &ldab,
		spannedGaps, &rows, &info);
	if(verbose)
	  fprintf(GlobalData->logfp, "dpbtrs (call2): rows " F_FTN_INT " bands " F_FTN_INT " ldab " F_FTN_INT " nrhs " F_FTN_INT " info " F_FTN_INT "\n",
		  rows, bands, ldab, nrhs, info);
	if(info < 0){
	  freeRecomputeData(&data);
	  assert(0 /* RECOMPUTE_LAPACK */);
	  return (RECOMPUTE_LAPACK);
	}else if(info > 0){
	  freeRecomputeData(&data);
	  assert(0 /* RECOMPUTE_LAPACK */);
	  return (RECOMPUTE_LAPACK);
	}
	for(gapPtr = spannedGaps, gapEnd = gapPtr + numComputeGaps,
	      gapPtr2 = gapVariance;
	    gapPtr < gapEnd; gapPtr++, gapPtr2++){
	  /* According to equation 5-7 we need to square the derivative and
	     multiply by the total gap variance for this clone but instead
	     we end up dividing by the total gap variance for this clone
	     because we neglected to divide by it before squaring and so
	     the net result is to need to divide by it. */
	  double term;
	  term = *gapPtr;
	  term *= term;
	  term /= cloneVariance[indexClones];
	  *gapPtr2 += term;
	}
      }
    }
    
    {
      int gapIndex, computeGapIndex;
      for(gapIndex = 0; gapIndex < numComputeGaps; gapIndex++){
	gapSize[computeGapsToGaps[gapIndex]] = gapConstants[gapIndex];
	gapSizeVariance[computeGapsToGaps[gapIndex]] = gapVariance[gapIndex];
	if(verbose){
	  fprintf(GlobalData->logfp,"GapSize(%d:%d) %f:%f\n", gapIndex,
		  computeGapsToGaps[gapIndex], gapConstants[gapIndex],
		  sqrt(gapVariance[gapIndex]));
	}
      }
          
      if(forceNonOverlaps){
	int alternate;
        
	hardConstraintSet = FALSE;
	InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
	for(gapIndex = 0, computeGapIndex = 0,
	      prevCI = NextCIScaffoldTIterator(&CIs);
            (thisCI = NextCIScaffoldTIterator(&CIs)) != NULL;
	    prevCI = thisCI, gapIndex++){
	  ChunkOrientationType edgeOrient;
	  float chiSquaredValue;
	  EdgeCGW_T *overlapEdge;
	  if(gapsToComputeGaps[gapIndex] == NULLINDEX){
	    continue;
	  }
	  if(gapSize[gapIndex] >= - CGW_MISSED_OVERLAP){
	    computeGapsToGaps[computeGapIndex] = gapIndex;
	    gapsToComputeGaps[gapIndex] = computeGapIndex;
	    computeGapIndex++;
	    continue;
	  }
	  if(GetNodeOrient(thisCI) == A_B){
	    if(GetNodeOrient(prevCI) == A_B){
	      edgeOrient = AB_AB;
	    }else{//GetNodeOrient(prevCI) == B_A
	      edgeOrient = BA_AB;
	    }
	  }else{//GetNodeOrient(thisCI) == B_A
	    if(GetNodeOrient(prevCI) == A_B){
	      edgeOrient = AB_BA;
	    }else{//GetNodeOrient(prevCI) == B_A
	      edgeOrient = BA_BA;
	    }
	  }
	  overlapEdge = FindOverlapEdgeChiSquare(graph, prevCI, thisCI->id,
                                                 edgeOrient, gapSize[gapIndex],
                                                 gapSizeVariance[gapIndex],
                                                 &chiSquaredValue,
                                                 (float)PAIRWISECHI2THRESHOLD_CGW,
                                                 &alternate, verbose);
          
	  if(overlapEdge && alternate){ // found a node that is out of order!!!!
#if 0
	    // Fix up the positions and return order problem --> recompute
	    FixUpMisorderedContigs(scaffold, prevCI, thisCI, edgeOrient, gapSize[gapIndex], gapSizeVariance[gapIndex], overlapEdge);
#endif
	    fprintf(stderr,"*** Least Squares found the following alternate edge...contig NOW!\n");
	    PrintGraphEdge(stderr, ScaffoldGraph->ContigGraph, " alternateEdge: ", overlapEdge, overlapEdge->idA);
            
	    // We want to merge the two contigs
	    // immediately, since these are problematic, but we know we want to overlap these guys.
	    ContigContainment(scaffold, prevCI, thisCI, overlapEdge, TRUE); // see CIScaffold_Cleanup_CGW.c
	    return RECOMPUTE_CONTIGGED_CONTAINMENTS;
	    //	    return RECOMPUTE_FAILED_REORDER_NEEDED;
	  }
	  if(overlapEdge && isContainmentEdge(overlapEdge)){
	    fprintf(stderr,"*** Least Squares found the following containment edge...contig NOW!\n");
	    fprintf(stderr,"*** " F_CID " (length %g) should be contained within " F_CID " (length %g)\n",
                    thisCI->id, thisCI->bpLength.mean,
                    prevCI->id, prevCI->bpLength.mean);
	    PrintGraphEdge(stderr, ScaffoldGraph->ContigGraph, " overlapEdge: ", overlapEdge, overlapEdge->idA);
            
	    // When we find a containment relationship in a scaffold we want to merge the two contigs
	    // immediately, since containments have the potential to induce situations that are confusing
	    // for least squares
	    ContigContainment(scaffold, prevCI, thisCI, overlapEdge, TRUE); // see CIScaffold_Cleanup_CGW.c
	    return RECOMPUTE_CONTIGGED_CONTAINMENTS;
	  }
          
          
	  if(overlapEdge == (EdgeCGW_T *)NULL){
            // gapsize is negative, so is leftedge
            double leftEdge = (gapSize[gapIndex] - 3 * sqrt(gapSizeVariance[gapIndex]));
            double newLeftEdge = leftEdge;
            double newStd = (newLeftEdge - (-CGW_MISSED_OVERLAP))/3.0;
            double newVariance = newStd * newStd;
            
            if(verbose){
              fprintf(GlobalData->stderrc,"GapChange Gap(%d:%d) CIs: " F_CID "," F_CID " new:(%f,%f) old(%f,%f)\n",
		      gapsToComputeGaps[gapIndex], gapIndex,
		      prevCI->id, thisCI->id,
		      (float)(- CGW_MISSED_OVERLAP), newVariance,
		      gapSize[gapIndex], gapSizeVariance[gapIndex]);
            }
	    //
	    // Adjust the gap variance so that the least squares computed mean position is within
	    // 3 sigma of the CGW_MISSED_OVERLAP positioning.  Otherwise, mate-induced edges between
	    // scaffold neighbors are not X-squared compatible with the scaffold positioning, and are
	    // not included in the set of edges used for subsequence scaffold construction.
	    //    ---------------------------
	    //               -------------------------------------------
	    //                  adjustment
	    //      |        |<---------->|
	    //      |     original      forced
	    //      |      position      position
	    //      | 
	    //   original left edge of 3-sigma interval
	    // goal is to adjust forced position variance, so the original left edge of 3 sigma interval
	    // is within an adjusted 3 sigma range of the new mean position.
	    
	    
	    gapSize[gapIndex] = - CGW_MISSED_OVERLAP;
	    gapSizeVariance[gapIndex] = newVariance;
	    gapsToComputeGaps[gapIndex] = NULLINDEX;
	    hardConstraintSet = TRUE;
	    continue;
	  }
	  if(abs(overlapEdge->distance.mean - gapSize[gapIndex]) <=
	     MAX_OVERLAP_SLOP_CGW){
	    computeGapsToGaps[computeGapIndex] = gapIndex;
	    gapsToComputeGaps[gapIndex] = computeGapIndex;
	    computeGapIndex++;
	    continue;
	  }
	  if(verbose){
	    fprintf(GlobalData->logfp,"GapChange(%d:%d) %f:%f\n",
		    gapsToComputeGaps[gapIndex], gapIndex,
		    overlapEdge->distance.mean, gapSize[gapIndex]);
	  }
	  gapSize[gapIndex] = overlapEdge->distance.mean;
	  gapsToComputeGaps[gapIndex] = NULLINDEX;
	  hardConstraintSet = TRUE;
	}
	numComputeGaps = computeGapIndex;
	if(hardConstraintSet){
	  double *gapEnd, *gapPtr;
	  for(gapPtr = gapConstants, gapEnd = gapPtr + numComputeGaps;
	      gapPtr < gapEnd; gapPtr++){
	    *gapPtr = 0.0;
	  }
	  for(gapPtr = gapCoefficients,
		gapEnd = gapPtr + (maxDiagonals * numComputeGaps);
	      gapPtr < gapEnd; gapPtr++){
	    *gapPtr = 0.0;
	  }
	  for(gapPtr = gapVariance, gapEnd = gapPtr + numComputeGaps;
	      gapPtr < gapEnd; gapPtr++){
	    *gapPtr = 0.0;
	  }
	}
      }
    }
  }while(forceNonOverlaps && hardConstraintSet && (numComputeGaps > 0));
  
  if(verbose)
    fprintf(GlobalData->logfp,"LSE: %f,%f #clones: %d,%d\n",
	    scaffold->info.Scaffold.leastSquareError, squaredError,
	    scaffold->info.Scaffold.numLeastSquareClones, numClones);
  scaffold->info.Scaffold.leastSquareError = squaredError;
  scaffold->info.Scaffold.numLeastSquareClones = numClones;
  {// Start
    
    double *gapPtr, *gapVarPtr;
    LengthT *prevLeftEnd, *prevRightEnd, *thisLeftEnd, *thisRightEnd;
    
    InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
    
    if(verbose)
      fprintf(GlobalData->logfp,"Reestimate gaps for scaffold\n");
    prevCI = NextCIScaffoldTIterator(&CIs);
    if(GetNodeOrient(prevCI) == A_B){
      prevLeftEnd = &(prevCI->offsetAEnd);
      prevRightEnd = &(prevCI->offsetBEnd);
    }else{
      prevLeftEnd = &(prevCI->offsetBEnd);
      prevRightEnd = &(prevCI->offsetAEnd);
    }
    if(verbose)
      fprintf(GlobalData->logfp, "Old %f,%f ",
	      prevLeftEnd->mean, sqrt(prevLeftEnd->variance));
    prevLeftEnd->mean = 0.0;
    prevLeftEnd->variance = 0.0;
    if(verbose)
      fprintf(GlobalData->logfp, "New %f,%f\n",
	      prevLeftEnd->mean, sqrt(prevLeftEnd->variance));
    
    for(gapPtr = gapSize, gapVarPtr = gapSizeVariance;
	(thisCI = NextCIScaffoldTIterator(&CIs)) != NULL;
	prevCI = thisCI, prevLeftEnd = thisLeftEnd,
	  prevRightEnd = thisRightEnd, gapPtr++, gapVarPtr++){
      LengthT gapDistance;
      CDS_COORD_t realDistance;
      
      if(GetNodeOrient(thisCI) == A_B){
	thisLeftEnd = &(thisCI->offsetAEnd);
	thisRightEnd = &(thisCI->offsetBEnd);
      }else{
	thisLeftEnd = &(thisCI->offsetBEnd);
	thisRightEnd = &(thisCI->offsetAEnd);
      }
      
      // Keep track of the biggest offset we've seen so far
      if(!maxOffset || 
	 maxOffset->mean < thisRightEnd->mean){
	maxOffset = thisRightEnd;
      }	
      
      
      gapDistance.mean = thisLeftEnd->mean - prevRightEnd->mean;
      gapDistance.variance = thisLeftEnd->variance - prevRightEnd->variance;
      if(verbose)
	fprintf(GlobalData->logfp, "Old %f,%f ",
		prevRightEnd->mean, sqrt(prevRightEnd->variance));
      prevRightEnd->mean = prevLeftEnd->mean + prevCI->bpLength.mean;
      prevRightEnd->variance = prevLeftEnd->variance +
	prevCI->bpLength.variance;
      if(verbose){
	fprintf(GlobalData->logfp, "New %f,%f\n",
		prevRightEnd->mean, sqrt(prevRightEnd->variance));
	fprintf(GlobalData->logfp, "Old %f,%f ",
		thisLeftEnd->mean, sqrt(thisLeftEnd->variance));
      }
      thisLeftEnd->mean = prevRightEnd->mean + *gapPtr;
      thisLeftEnd->variance = prevRightEnd->variance + *gapVarPtr;
      if(verbose)
	fprintf(GlobalData->logfp, "New %f,%f\n",
		thisLeftEnd->mean, sqrt(thisLeftEnd->variance));
      if(GetNodeOrient(thisCI) == A_B){
	if(GetNodeOrient(prevCI) == A_B){
	  realDistance = thisCI->aEndCoord - prevCI->bEndCoord;
	}else{//GetNodeOrient(prevCI) == B_A
	  realDistance = thisCI->aEndCoord - prevCI->aEndCoord;
	}
      }else{//GetNodeOrient(thisCI) == B_A
	if(GetNodeOrient(prevCI) == A_B){
	  realDistance = thisCI->bEndCoord - prevCI->bEndCoord;
	}else{//GetNodeOrient(prevCI) == B_A
	  realDistance = thisCI->bEndCoord - prevCI->aEndCoord;
	}
      }
      if(verbose)
	fprintf(GlobalData->logfp, "Old %f New %f Real " F_COORD " StdDev %f,%f\n",
		gapDistance.mean, *gapPtr, realDistance,
		sqrt(gapDistance.variance), sqrt(*gapVarPtr));
      
    }
    if(verbose)
      fprintf(GlobalData->logfp, "Old %f,%f ",
	      prevRightEnd->mean, sqrt(prevRightEnd->variance));
    prevRightEnd->mean = prevLeftEnd->mean + prevCI->bpLength.mean;
    prevRightEnd->variance = prevLeftEnd->variance +
      prevCI->bpLength.variance;
    if(verbose)
      fprintf(GlobalData->logfp, "New %f,%f\n",
	      prevRightEnd->mean, sqrt(prevRightEnd->variance));
    
#if 0
    fprintf(stderr,"Scaffold " F_CID " Length %f,%f-%f,%f\n",
	    scaffold->id,
            scaffold->bpLength.mean, sqrt(scaffold->bpLength.variance),
	    prevRightEnd->mean, sqrt(prevRightEnd->variance));
#endif
    fprintf(GlobalData->logfp,"Scaffold " F_CID " Length %f,%f-%f,%f\n",
	    scaffold->id,
            scaffold->bpLength.mean, sqrt(scaffold->bpLength.variance),
	    maxOffset->mean, sqrt(maxOffset->variance));
    //      DumpCIScaffold(graph, scaffold, FALSE);
    scaffold->bpLength = *maxOffset;
    
    SetCIScaffoldTLength(ScaffoldGraph, scaffold, TRUE); // recompute scaffold length, just to be sure
    
  }
  
  StopTimerT(&GlobalData->RecomputeOffsetsTimer);   /*  STOP */
  CheckScaffoldGraphCache(ScaffoldGraph); // flush the cache if it has gotten too big
  
  freeRecomputeData(&data);
  return (RECOMPUTE_OK);
}

void MarkInternalEdgeStatus(ScaffoldGraphT *graph, CIScaffoldT *scaffold, 
                            float pairwiseChiSquaredThreshhold,
                            float maxVariance,
                            int markTrusted, int markUntrusted,
                            int doNotChange,
                            int operateOnMerged){
  CIScaffoldTIterator CIs;
  /* Iterate over all of the CIEdges */
  GraphEdgeIterator edges;
  EdgeCGW_T *edge;
  NodeCGW_T *thisCI;
  int32 numCIs;
  int32 indexCIs;
  int internalEdges = 0;  /* Number of merged edges (not including UNTRUSTED)
                             that are internal to scaffold */
  int confirmedInternalEdges = 0; /* Number of merged edges confirmed by
                                     current CI positions */
  
#undef DEBUG_MARKINTERNAL
#ifdef DEBUG_MARKINTERNAL
  fprintf(GlobalData->logfp, "Marking Edges for Scaffold " F_CID "\n",
	  scaffold->id);
#endif
  InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
  
  for(indexCIs = 0;
      (thisCI = NextCIScaffoldTIterator(&CIs)) != NULL;){
    thisCI->indexInScaffold = indexCIs;
    indexCIs++;
  }
  numCIs = indexCIs;
  if(numCIs != scaffold->info.Scaffold.numElements){
    fprintf(GlobalData->logfp, "NumElements inconsistent %d,%d\n",
	    numCIs, scaffold->info.Scaffold.numElements);
    scaffold->info.Scaffold.numElements = numCIs;
  }
  
  assert(indexCIs == numCIs);
  
  InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
  
  while((thisCI = NextCIScaffoldTIterator(&CIs)) != NULL){
    int flags = GRAPH_EDGE_DEFAULT;
    
    if(!operateOnMerged)
      flags |= GRAPH_EDGE_RAW_ONLY;
    
    InitGraphEdgeIterator(ScaffoldGraph->RezGraph, thisCI->id, 
			  ALL_END,
			  ALL_EDGES, 
			  flags,
			  &edges);// Use merged edges
    
    while((edge = NextGraphEdgeIterator(&edges))!= NULL){
      int isA = (edge->idA == thisCI->id);
      NodeCGW_T *otherCI =
	GetGraphNode(ScaffoldGraph->RezGraph,
                     (isA? edge->idB: edge->idA));
      ChunkOrientationType edgeOrient;
      FragOrient thisCIorient, otherCIorient;
      LengthT gapDistance;
      float chiSquareResult;


#if DEBUG_MARKINTERNAL > 1
      fprintf(GlobalData->logfp, " examining edge [" F_CID "," F_CID "] (%f,%f) weight %d ori: %c\n",
	      thisCI->id, otherCI->id,
	      edge->distance.mean, edge->distance.variance,
	      edge->edgesContributing,
	      GetEdgeOrientationWRT(edge, thisCI->id));
#endif      
      /* We do not want to change the labels for edges with certain
	 labels as specified in the doNotChange mask. */
      if(edge->flags.bits.edgeStatus & doNotChange){
	continue;
      }
      /* We only want to label edges between CIs in the same scaffold. */
      if(otherCI->scaffoldID != thisCI->scaffoldID){
	SetEdgeStatus(graph->RezGraph, edge, INTER_SCAFFOLD_EDGE_STATUS);
	continue;
      }
      /* We only want to label an edge once and the
	 iterator will visit intra scaffold edges twice so we use
	 the condition that the edge is canonical - that is the
	 condition thisCI->indexInScaffold < otherCI->indexInScaffold
	 to guarantee labelling the edge only once. */
      if(otherCI->indexInScaffold <= thisCI->indexInScaffold){
	continue;
      }
      
      /* Check that the edge orientation is consistent with the CI positions
	 and orientations within the scaffold. */
      edgeOrient = GetEdgeOrientationWRT(edge, thisCI->id);
      switch(edgeOrient){
        case AB_BA:
          //      thisCI                                        otherCI
          //  A --------------------- B               B --------------------- A
          //    5'----->                                           <------5'
          thisCIorient = A_B;
          otherCIorient = B_A;
          gapDistance.mean = otherCI->offsetBEnd.mean - thisCI->offsetBEnd.mean;
          gapDistance.variance = otherCI->offsetBEnd.variance -
            thisCI->offsetBEnd.variance;
          break;
        case AB_AB:
          //      thisCI                                        otherCI
          //  A --------------------- B               A --------------------- B
          //    5'----->                                           <------5'
          thisCIorient = A_B;
          otherCIorient = A_B;
          gapDistance.mean = otherCI->offsetAEnd.mean - thisCI->offsetBEnd.mean;
          gapDistance.variance = otherCI->offsetAEnd.variance -
            thisCI->offsetBEnd.variance;
          break;
        case BA_BA:
          //      thisCI                                        otherCI
          //  B --------------------- A               B --------------------- A
          //    5'----->                                           <------5'
          thisCIorient = B_A;
          otherCIorient = B_A;
          gapDistance.mean = otherCI->offsetBEnd.mean - thisCI->offsetAEnd.mean;
          gapDistance.variance = otherCI->offsetBEnd.variance -
            thisCI->offsetAEnd.variance;
          break;
        case BA_AB:
          //      thisCI                                        otherCI
          //  B --------------------- A               A --------------------- B
          //    5'----->                                           <------5'
          thisCIorient = B_A;
          otherCIorient = A_B;
          gapDistance.mean = otherCI->offsetAEnd.mean - thisCI->offsetAEnd.mean;
          gapDistance.variance = otherCI->offsetAEnd.variance -
            thisCI->offsetAEnd.variance;
          break;
        default:
          assert(0);
          break;
      }
      if((GetNodeOrient(thisCI) != thisCIorient) ||
	 (GetNodeOrient(otherCI) != otherCIorient)){
	/* Mark as untrusted an edge whose orientation does not agree
	   with the orientation of the CIs in the scaffold. */
#ifdef DEBUG_MARKINTERNAL
	fprintf(GlobalData->logfp, "[" F_CID "," F_CID "]Bad orientation (%c,%c) (%c,%c)\n",
		thisCI->id, otherCI->id,
		GetNodeOrient(thisCI), thisCIorient, GetNodeOrient(otherCI),
		otherCIorient);
#endif
	SetEdgeStatus(graph->RezGraph, edge, markUntrusted ? UNTRUSTED_EDGE_STATUS :
                      TENTATIVE_UNTRUSTED_EDGE_STATUS);
	continue;
      }
      if(gapDistance.variance <= 0.0){
	/* This condition should not occur but when it does it causes an
	   assert in the PairwiseChiSquare subroutine so we will just mark
	   the edge as untrusted so the program can keep going but this
	   needs to be investigated and fixed!!!!
	*/

	fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Bad Gap Variance (%f,%f) (%f,%f) DANGER WILL ROBINSON!!!\n",
		thisCI->id, thisCI->scaffoldID, otherCI->id,
		otherCI->scaffoldID,
		gapDistance.mean, gapDistance.variance,
		edge->distance.mean, edge->distance.variance);
	DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,scaffold,TRUE);
	DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,scaffold,FALSE);

	SetEdgeStatus(graph->RezGraph, edge, markUntrusted ? UNTRUSTED_EDGE_STATUS :
                      TENTATIVE_UNTRUSTED_EDGE_STATUS);
	continue;
      }
      if(!PairwiseChiSquare((float)gapDistance.mean,
			    gapDistance.variance,
			    (float)edge->distance.mean,
			    edge->distance.variance,
			    (LengthT *)NULL, &chiSquareResult,
			    pairwiseChiSquaredThreshhold)){
	/* Mark  this edge as untrusted if the distance of the edge is not
	   consistent with the estimated gap distance as judged by the
	   Chi Squared Test. */
#ifdef DEBUG_MARKINTERNAL
	fprintf(GlobalData->logfp, "[" F_CID "," F_CID "]Bad Chi Squared %f (%f,%f) (%f,%f)\n",
		thisCI->id, otherCI->id,
		chiSquareResult, gapDistance.mean, gapDistance.variance,
		edge->distance.mean, edge->distance.variance);
#endif
	SetEdgeStatus(graph->RezGraph, edge, markUntrusted ? UNTRUSTED_EDGE_STATUS :
                      TENTATIVE_UNTRUSTED_EDGE_STATUS);
	continue;
      }
#ifdef DEBUG_MARKINTERNAL
      fprintf(GlobalData->logfp, "[" F_CID "," F_CID "]Good Chi Squared %f (%f,%f) (%f,%f)\n",
	      thisCI->id, otherCI->id,
	      chiSquareResult, gapDistance.mean, gapDistance.variance,
	      edge->distance.mean, edge->distance.variance);
#endif
      if(edge->distance.variance > maxVariance){
#ifdef DEBUG_MARKINTERNAL
	fprintf(GlobalData->logfp, "[" F_CID "," F_CID "]Variance too large %f\n",
		thisCI->id, otherCI->id,
		edge->distance.variance);
#endif
	SetEdgeStatus(graph->RezGraph,edge, LARGE_VARIANCE_EDGE_STATUS);
	continue;
      }
      SetEdgeStatus(graph->RezGraph,edge, markTrusted ? TRUSTED_EDGE_STATUS :
                    TENTATIVE_TRUSTED_EDGE_STATUS);
    }
  }
  
  InitCIScaffoldTIterator(graph, scaffold, TRUE, FALSE, &CIs);
  
  while((thisCI = NextCIScaffoldTIterator(&CIs)) != NULL){
    
    InitGraphEdgeIterator(ScaffoldGraph->RezGraph, thisCI->id, 
			  ALL_END,
			  ALL_EDGES, 
			  GRAPH_EDGE_DEFAULT,
			  &edges);// Use merged edges
    
    while((edge = NextGraphEdgeIterator(&edges))!= NULL){
      int isA = (edge->idA == thisCI->id);
      NodeCGW_T *otherCI =
	GetGraphNode(ScaffoldGraph->RezGraph,
                     (isA? edge->idB: edge->idA));
      EdgeStatus edgeStatus;
      
      if(otherCI->indexInScaffold <= thisCI->indexInScaffold){
	continue; // Only interested in looking at an edge once
      }
      edgeStatus = GetEdgeStatus(edge);
      if((edgeStatus == TRUSTED_EDGE_STATUS) ||
	 (edgeStatus == TENTATIVE_TRUSTED_EDGE_STATUS)){
	internalEdges++;
	confirmedInternalEdges++;
      }else if(edgeStatus == TENTATIVE_UNTRUSTED_EDGE_STATUS){
	internalEdges++;
      }
    }
  }
#ifdef DEBUG_MARKINTERNAL
  fprintf(GlobalData->logfp,"#Internal Edges %d,%d confirmed %d,%d\n",
	  scaffold->info.Scaffold.internalEdges, internalEdges,
	  scaffold->info.Scaffold.confirmedInternalEdges, confirmedInternalEdges);
#endif
  scaffold->info.Scaffold.internalEdges = internalEdges;
  scaffold->info.Scaffold.confirmedInternalEdges = confirmedInternalEdges;
  return;
}


static int EdgeAndGapAreVaguelyCompatible(double distDiff,
				     double diffVar,
				     double maxDiffSlop,
				     double maxSigmaSlop){
  if(distDiff>maxDiffSlop)return FALSE;
  if(diffVar<0){
    fprintf(stderr,"WARNING: variance difference is negative -- probably trouble with variances after interleaving\n"); 
  }
  if(abs(distDiff)/sqrt(abs(diffVar))>maxSigmaSlop)return FALSE;
  return TRUE;
}


/// New test code to help handle slightly messier cases!
int IsInternalEdgeStatusVaguelyOK(EdgeCGW_T *edge,CDS_CID_t thisCIid){

  NodeCGW_T *thisCI = GetGraphNode(ScaffoldGraph->RezGraph,thisCIid);
  GraphEdgeIterator edges;

  // WE ASSUME edge COMES FROM SOMETHING LIKE THE FOLLOWING:
  /*

  InitGraphEdgeIterator(ScaffoldGraph->RezGraph, thisCI->id, 
			ALL_END,
			ALL_EDGES, 
			GRAPH_EDGES_RAW_ONLY,
			&edges); 

  while((edge = NextGraphEdgeIterator(&edges))!= NULL){

  */
    
  int isA = (edge->idA == thisCI->id);
  NodeCGW_T *otherCI =
    GetGraphNode(ScaffoldGraph->RezGraph,
		 (isA? edge->idB: edge->idA));
  ChunkOrientationType edgeOrient;
  FragOrient thisCIorient, otherCIorient;
  LengthT gapDistance;
  float chiSquareResult;

  /* We only want to label edges between CIs in the same scaffold; this is up to the caller. */
  if(otherCI->scaffoldID != thisCI->scaffoldID){
    assert(0);
  }

  /* We only want to do this to an edge once; the calling routine should
     ensure that the edge is canonical - that is the
     condition thisCI->indexInScaffold < otherCI->indexInScaffold;
     it would probably be harmless to allow this through, but why bother to let the 
     programmer off easy?
  */
  if(otherCI->indexInScaffold <= thisCI->indexInScaffold){
    assert(0);
  }
      
  /* Check that the edge orientation is consistent with the CI positions
     and orientations within the scaffold. */
  edgeOrient = GetEdgeOrientationWRT(edge, thisCI->id);
  switch(edgeOrient){
  case AB_BA:
    //      thisCI                                        otherCI
    //  A --------------------- B               B --------------------- A
    //    5'----->                                           <------5'
    thisCIorient = A_B;
    otherCIorient = B_A;
    gapDistance.mean = otherCI->offsetBEnd.mean - thisCI->offsetBEnd.mean;
    gapDistance.variance = otherCI->offsetBEnd.variance -
      thisCI->offsetBEnd.variance;
    break;
  case AB_AB:
    //      thisCI                                        otherCI
    //  A --------------------- B               A --------------------- B
    //    5'----->                                           <------5'
    thisCIorient = A_B;
    otherCIorient = A_B;
    gapDistance.mean = otherCI->offsetAEnd.mean - thisCI->offsetBEnd.mean;
    gapDistance.variance = otherCI->offsetAEnd.variance -
      thisCI->offsetBEnd.variance;
    break;
  case BA_BA:
    //      thisCI                                        otherCI
    //  B --------------------- A               B --------------------- A
    //    5'----->                                           <------5'
    thisCIorient = B_A;
    otherCIorient = B_A;
    gapDistance.mean = otherCI->offsetBEnd.mean - thisCI->offsetAEnd.mean;
    gapDistance.variance = otherCI->offsetBEnd.variance -
      thisCI->offsetAEnd.variance;
    break;
  case BA_AB:
    //      thisCI                                        otherCI
    //  B --------------------- A               A --------------------- B
    //    5'----->                                           <------5'
    thisCIorient = B_A;
    otherCIorient = A_B;
    gapDistance.mean = otherCI->offsetAEnd.mean - thisCI->offsetAEnd.mean;
    gapDistance.variance = otherCI->offsetAEnd.variance -
      thisCI->offsetAEnd.variance;
    break;
  default:
    assert(0);
    break;
  }
  if((GetNodeOrient(thisCI) != thisCIorient) ||
     (GetNodeOrient(otherCI) != otherCIorient)){
    /* edge orientation does not agree
       with the orientation of the CIs in the scaffold. */
#ifdef DEBUG_MARKINTERNAL
    fprintf(GlobalData->logfp, "[" F_CID "," F_CID "]Bad orientation (%c,%c) (%c,%c)\n",
	    thisCI->id, otherCI->id,
	    GetNodeOrient(thisCI), thisCIorient, GetNodeOrient(otherCI),
	    otherCIorient);
#endif
    return FALSE;
  }
  if(gapDistance.variance <= 0.0){
    /* This condition should not occur, so kill it now!
    */

    fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Bad Gap Variance (%f,%f) (%f,%f) DANGER WILL ROBINSON!!!\n",
	    thisCI->id, thisCI->scaffoldID, otherCI->id,
	    otherCI->scaffoldID,
	    gapDistance.mean, gapDistance.variance,
	    edge->distance.mean, edge->distance.variance);
    DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,GetGraphNode(ScaffoldGraph->ScaffoldGraph,thisCI->scaffoldID),TRUE);
    DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,GetGraphNode(ScaffoldGraph->ScaffoldGraph,thisCI->scaffoldID),FALSE);


#ifdef NEG_GAP_VARIANCE_PROBLEM_FIXED
    assert(0);
#endif
  }
  if(edge->distance.variance <= 0.0){
    /* This condition should not occur, so kill it now!
    */

    fprintf(GlobalData->logfp, "[" F_CID "." F_CID "," F_CID "." F_CID "]Bad Edge Variance (%f,%f) (%f,%f) DANGER WILL ROBINSON!!!\n",
	    thisCI->id, thisCI->scaffoldID, otherCI->id,
	    otherCI->scaffoldID,
	    gapDistance.mean, gapDistance.variance,
	    edge->distance.mean, edge->distance.variance);
    DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,GetGraphNode(ScaffoldGraph->ScaffoldGraph,thisCI->scaffoldID),TRUE);
    DumpACIScaffoldNew(GlobalData->logfp,ScaffoldGraph,GetGraphNode(ScaffoldGraph->ScaffoldGraph,thisCI->scaffoldID),FALSE);

    assert(0);
  }
  if(!EdgeAndGapAreVaguelyCompatible((double)gapDistance.mean-edge->distance.mean,
				(double)gapDistance.variance+edge->distance.variance,
				MAX_ABSOLUTE_SLOP,
				MAX_SIGMA_SLOP)){
    /* Mark  this edge as untrusted if the distance of the edge is not
       consistent with the estimated gap distance as judged by the
       Chi Squared Test. */
#ifdef DEBUG_MARKINTERNAL
    fprintf(GlobalData->logfp, "[" F_CID "," F_CID "]Not vaguely compatible %f (%f,%f) (%f,%f)\n",
	    thisCI->id, otherCI->id,
	    chiSquareResult, gapDistance.mean, gapDistance.variance,
	    edge->distance.mean, edge->distance.variance);
#endif
    return FALSE;
  }

#ifdef DEBUG_MARKINTERNAL
  fprintf(GlobalData->logfp, "[" F_CID "," F_CID "]At least vaguely compatible %f (%f,%f) (%f,%f)\n",
	  thisCI->id, otherCI->id,
	  chiSquareResult, gapDistance.mean, gapDistance.variance,
	  edge->distance.mean, edge->distance.variance);
#endif

  return TRUE;
}



void LeastSquaresGapEstimates(ScaffoldGraphT *graph, int markEdges,
			      int useGuides, int forceNonOverlaps,
			      int checkConnectivity, int verbose){
  
  RecomputeOffsetsStatus status;
  GraphNodeIterator scaffolds;
  CIScaffoldT *scaffold = NULL;
  int numScaffolds;
  int redo = FALSE;
  int cnt = 0;
  
  fprintf(stderr,"* Start of LSGapEstimates markEdges:%d useGuides:%d*\n",
	  markEdges, useGuides);
  if(!markEdges){ // if we are marking edges, we don't need to check now
    CheckAllTrustedEdges(graph);
    fprintf(stderr,"****\n");
  }
  
  numScaffolds = GetNumGraphNodes(graph->ScaffoldGraph);
  InitGraphNodeIterator(&scaffolds, graph->ScaffoldGraph, GRAPH_NODE_DEFAULT);
  while((scaffold = (redo?scaffold:NextGraphNodeIterator(&scaffolds))) != NULL){
    if(isDeadCIScaffoldT(scaffold) || scaffold->type != REAL_SCAFFOLD){
      assert(!redo);
      continue;
    }
    if(++cnt % 10000 == 0){
      fprintf(GlobalData->stderrc," LeastSquaresGapEstimates %d   scaffold " F_CID "/%d\n",
	      cnt - 1, scaffold->id, numScaffolds);
      fflush(GlobalData->stderrc);
    }
    redo = FALSE;
    if(markEdges){
      //  float maxVariance = useGuides ? 100,000,000,000.0 : 30,000,000.0;
      float maxVariance = (useGuides ? (1000.0 * SLOPPY_EDGE_VARIANCE_THRESHHOLD) : SLOPPY_EDGE_VARIANCE_THRESHHOLD) ;
      
      MarkInternalEdgeStatus(graph, scaffold, PAIRWISECHI2THRESHOLD_CGW,
			     maxVariance, TRUE, TRUE, 0, TRUE);
    }
    // Check that the scaffold is connected by trusted edges - otherwise
    // RecomputeOffsetsInScaffold will fail due to a singularity in the matrix
    
    // that it needs to invert - if it is not, break it into a set of maximal
    // scaffolds which are connected.
    if(checkConnectivity){
      CDS_CID_t  scaffid = scaffold -> id;
      int numComponents = CheckScaffoldConnectivityAndSplit(graph,scaffold, ALL_TRUSTED_EDGES, verbose);
      
      if(numComponents > 1){ // we split the scaffold because it wasn't connected
	fprintf(stderr,"* Scaffold not connected: Split scaffold " F_CID " into %d pieces\n",
		scaffid, numComponents);
	continue;
      }else{
	if(!IsScaffold2EdgeConnected(ScaffoldGraph, scaffold)){
	  fprintf(stderr,"*###### Scaffold " F_CID " is not 2-edge connected... SPLIT IT!\n",
		  scaffold->id);
	  numComponents = CheckScaffoldConnectivityAndSplit(ScaffoldGraph, scaffold, ALL_TRUSTED_EDGES, FALSE);
	  if(numComponents > 1){
	    fprintf(stderr,"* Scaffold not 2 edge-connected: Split scaffold " F_CID " into %d pieces\n",
		    scaffid, numComponents);
	    continue;
	  }
	}
      }
    }
    
    {
      int i;
      for(i = 0; i < 100; i++){
	CheckLSScaffoldWierdnesses("BEFORE", graph, scaffold);
	status =	RecomputeOffsetsInScaffold(graph, scaffold, TRUE, forceNonOverlaps, verbose);
	if(status == RECOMPUTE_CONTIGGED_CONTAINMENTS){ // We want to restart from the top of the loop, including edge marking
	  redo = TRUE;
	  break;
	}
	if(status != RECOMPUTE_FAILED_REORDER_NEEDED) // We want to simply try again, since we just changed the scaffold order
	  break;
	fprintf(stderr,"* RecomputeOffsetsInScaffold " F_CID " attempt %d failed...iterating\n", scaffold->id, i);
      }
    }
    
    
    CheckLSScaffoldWierdnesses("AFTER", graph, scaffold);
    
    
    if(status != RECOMPUTE_OK){
      fprintf(GlobalData->logfp, "RecomputeOffsetsInScaffold failed (%d) for scaffold " F_CID "\n",
	      status, scaffold->id);
      CheckCIScaffoldT(graph, scaffold);  // NEW
      
    }
  }
  return;
}


void  CheckLSScaffoldWierdnesses(char *string, ScaffoldGraphT *graph, CIScaffoldT *scaffold){
  CIScaffoldTIterator CIs;
  ChunkInstanceT *firstCI, *secondCI;
  int oops = 0;
  LengthT delta, *minOffsetp, *maxOffsetp;
  
  InitCIScaffoldTIterator(graph, scaffold, TRUE,  FALSE, &CIs);
  firstCI = NextCIScaffoldTIterator(&CIs);
  if(firstCI->offsetAEnd.mean < firstCI->offsetBEnd.mean){
    minOffsetp = &firstCI->offsetAEnd;;
    maxOffsetp = &firstCI->offsetBEnd;
  }else{
    minOffsetp = &firstCI->offsetBEnd;;
    maxOffsetp = &firstCI->offsetAEnd;
  }
  
  if(minOffsetp->mean < 0.0){
    delta.mean = -minOffsetp->mean;
    delta.variance = minOffsetp->variance;
    fprintf(GlobalData->stderrc,"*****SQUAWK1!!!!  CheckLSScaffoldWierdnesses %s for scaffold " F_CID ", shifting by (%g,%g)\n...Fixing...\n",
            string, scaffold->id,
            delta.mean, delta.variance);
    DumpCIScaffold(GlobalData->stderrc,graph, scaffold, FALSE);
    minOffsetp->mean = minOffsetp->variance = 0.0;
    maxOffsetp->mean += delta.mean;
    maxOffsetp->variance += -delta.variance;
    oops = 1;
  }else if(minOffsetp->mean > 0.0){
    delta.mean = -minOffsetp->mean;
    delta.variance = -minOffsetp->variance;
    fprintf(GlobalData->stderrc,"*****SQUAWK2!!!!  CheckLSScaffoldWierdnesses %s for scaffold " F_CID ", shifting by (%g,%g)\n...Fixing...\n",
            string, scaffold->id,
            delta.mean, delta.variance);
    DumpCIScaffold(GlobalData->stderrc,graph, scaffold, FALSE);
    minOffsetp->mean = minOffsetp->variance = 0.0;
    maxOffsetp->mean += delta.mean;
    maxOffsetp->variance += delta.variance;
    oops = 1;
  }else{
    return; // we are done
  }
  secondCI = NextCIScaffoldTIterator(&CIs);
  
  if(secondCI == NULL){
    scaffold->bpLength = (firstCI->offsetAEnd.mean < firstCI->offsetBEnd.mean) ?
      firstCI->offsetBEnd : firstCI->offsetAEnd;
    return;
  }
  
  AddDeltaToScaffoldOffsets(graph, scaffold->id,  secondCI->id, TRUE, FALSE, delta);
  
  fprintf(GlobalData->stderrc, "Done!! Scaffold after is:\n"); 
  DumpCIScaffold(GlobalData->stderrc,graph, scaffold, FALSE);
  
}
