/*
 *  Copyright 2007-2016 The OpenMx Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */


#include "omxDefines.h"
#include "omxSymbolTable.h"
#include "omxData.h"
#include "omxFIMLFitFunction.h"
#include "matrix.h"
#include <Eigen/Cholesky>

#ifdef SHADOW_DIAG
#pragma GCC diagnostic warning "-Wshadow"
#endif

void omxFIMLAdvanceJointRow(int *row, int *numIdenticalDefs, 
	int *numIdenticalContinuousMissingness,
	int *numIdenticalOrdinalMissingness, 
	int *numIdenticalContinuousRows,
	omxFIMLFitFunction *obj, int numDefs, int numIdentical) {

	int rowVal = *row;

	auto& identicalDefs = obj->identicalDefs;
	auto& identicalMissingness = obj->identicalMissingness;
	auto& identicalRows = obj->identicalRows;

	if(*numIdenticalDefs <= 0)
		*numIdenticalDefs = identicalDefs[rowVal];
	if(*numIdenticalContinuousMissingness <= 0)
		*numIdenticalContinuousMissingness = identicalMissingness[rowVal];
	if(*numIdenticalOrdinalMissingness <= 0)
		*numIdenticalOrdinalMissingness = identicalMissingness[rowVal];
	if(*numIdenticalContinuousRows <= 0)
		*numIdenticalContinuousRows = identicalRows[rowVal];

	*numIdenticalDefs -= numIdentical;
	*numIdenticalContinuousMissingness -= numIdentical;
	*numIdenticalContinuousRows -= numIdentical;
	*numIdenticalOrdinalMissingness -= numIdentical;
}

bool oldByRow::eval()
{
	double Q = 0.0;
	int numIdenticalDefs = 0, numIdenticalOrdinalMissingness = 0,
		numIdenticalContinuousMissingness = 0, numIdenticalContinuousRows = 0;
	
	omxMatrix *smallRow, *smallCov, *smallMeans, *RCX;
	omxMatrix *ordMeans, *ordCov, *contRow;
	omxMatrix *halfCov, *reduceCov, *ordContCov;
	
	// Locals, for readability.  Compiler should cut through this.
	smallRow 	= ofo->smallRow;
	smallCov 	= ofo->smallCov;
	smallMeans	= ofo->smallMeans;
	ordMeans    = ofo->ordMeans;
	ordCov      = ofo->ordCov;
	contRow     = ofo->contRow;
	halfCov     = ofo->halfCov;
	reduceCov   = ofo->reduceCov;
	ordContCov  = ofo->ordContCov;
	RCX 		= ofo->RCX;
	
	int numDefs = data->defVars.size();
	
	Eigen::ArrayXi ordBuffer(dataColumns.size());
	
	Eigen::VectorXi ordRemove(cov->cols);
	Eigen::VectorXi contRemove(cov->cols);
	char u = 'U', l = 'L';
	int info;
	double determinant = 0.0;
	double oned = 1.0, zerod = 0.0, minusoned = -1.0;
	int onei = 1;
	double likelihood;
	
	int numContinuous = 0;
	int numOrdinal = 0;
	
	while(row < lastrow) {
		mxLogSetCurrentRow(row);
		int numIdentical = identicalRows[row];
		
		omxDataRow(data, indexVector[row], dataColumns, smallRow);
		
		//If the expectation is a state space model then
		// set the y attribute of the state space expectation to smallRow.
		
		if(numIdenticalDefs <= 0 || numIdenticalContinuousMissingness <= 0 || numIdenticalOrdinalMissingness <= 0 || 
		   firstRow) {  // If we're keeping covariance from the previous row, do not populate 
			// Handle Definition Variables.
			if((numDefs && numIdenticalDefs <= 0) || firstRow) {
				if(OMX_DEBUG_ROWS(row)) { mxLog("Handling Definition Vars."); }
				bool numVarsFilled = expectation->loadDefVars(indexVector[row]);
				if (numVarsFilled || firstRow) {
					omxExpectationCompute(fc, expectation, NULL);
				}
			}
			// Filter down correlation matrix and calculate thresholds.
			// TODO: If identical ordinal or continuous missingness, ignore only the appropriate columns.
			numContinuous = 0;
			numOrdinal = 0;
			for(int j = 0; j < dataColumns.size(); j++) {
				int var = dataColumns[j];
				// TODO: Might save time by preseparating ordinal from continuous.
				if (omxDataElementMissing(data, indexVector[row], var)) {
					ordRemove[j] = 1;
					contRemove[j] = 1;
					if(OMX_DEBUG_ROWS(row)) { 
						mxLog("Row %d, column %d : NA", row, j);
					}
					continue;
				}
				else if (omxDataColumnIsFactor(data, var)) {
					++numOrdinal;
					ordRemove[j] = 0;
					contRemove[j] = 1;
					if(OMX_DEBUG_ROWS(row)) { 
						mxLog("Row %d, column %d : Ordinal", row, j);
					}
				} 
				else {
					++numContinuous;
					ordRemove[j] = 1;
					contRemove[j] = 0;
					if(OMX_DEBUG_ROWS(row)) { 
						mxLog("Row %d, column %d : Continuous", row, j);
					}
				}
			}
			
			if(OMX_DEBUG_ROWS(row)) {
				mxLog("Removals: %d ordinal, %d continuous out of %d total.",
				      dataColumns.size() - numOrdinal, dataColumns.size() - numContinuous,
				      dataColumns.size());
			}

			if (thresholdsMat) {
				omxRecompute(thresholdsMat, fc);
				for(int j=0; j < dataColumns.size(); j++) {
					int var = dataColumns[j];
					if (!omxDataColumnIsFactor(data, var)) continue;
					if (!thresholdsIncreasing(thresholdsMat, thresholdCols[j].column,
								  thresholdCols[j].numThresholds, fc)) return true;
				}
			}
		} // keep covariance from previous row
			
			// TODO: Possible solution here: Manually record threshold column and index from data 
			//   during this initial reduction step.  Since all the rest is algebras, it'll filter 
			//   naturally.  Calculate offsets from continuous data, then dereference actual 
			//   threshold values from the threshold matrix in its original state.  
			//   Alternately, rearrange the thresholds matrix (and maybe data matrix) to split
			//    ordinal and continuous variables.
			//   Requirement: colNum integer vector
			
			if(numContinuous <= 0 && numOrdinal <= 0) {
				// All elements missing.  Skip row.
				omxFIMLAdvanceJointRow(&row, &numIdenticalDefs, 
				&numIdenticalContinuousMissingness,
				&numIdenticalOrdinalMissingness, 
				&numIdenticalContinuousRows,
						       shared_ofo, numDefs, numIdentical);
				recordRow(1.0);
				continue;
			}
			
			//  smallCov <- cov[!contRemove, !contRemove] : covariance of continuous elements
			//  smallMeans <- means[ALL, !contRemove] : continuous means
			//  smallRow <- data[ALL, !contRemove]  : continuous data
			//              ordCov <- cov[!ordRemove, !ordRemove]
			//              ordMeans <- means[NULL, !ordRemove]
			//              ordData <- data[NULL, !ordRemove]
			//              ordContCov <- cov[!contRemove, !ordRemove]
			
			// TODO: Data handling is confusing.  Maybe set two self-aliased row-reduction "datacolumns" elements?
			
			// SEPARATION: 
			// Catch here: If continuous columns are all missing, skip everything except the ordCov calculations
			//              in this case, log likelihood of the continuous is 1 (likelihood is 0)
			// Do not recompute ordcov if missingness is identical and no def vars
			
			// SEPARATION: 
			//  Unprojected covariances only need to reset and re-filter if there are def vars or the appropriate missingness pattern changes
			//  Also, if each one is not all-missing.
			
			if(numContinuous <= 0) {
				// All continuous missingness.  Populate some stuff.
				Q = 0.0;
				determinant = 0.0;
				if(numIdenticalDefs <= 0 || numIdenticalOrdinalMissingness <= 0 || firstRow) {
					// Recalculate Ordinal covariance matrix
					omxCopyMatrix(ordCov, cov);
					omxRemoveRowsAndColumns(ordCov, ordRemove.data(), ordRemove.data());

					EigenMatrixAdaptor EordCov(ordCov);
					ol.setCovariance(EordCov, fc);
					
					int ox=0;
					for(int j = 0; j < dataColumns.size(); j++) {
						if(ordRemove[j]) continue;         // NA or non-ordinal
						ordBuffer[ox] = j;
						ox += 1;
					}
					Eigen::Map< Eigen::ArrayXi > ordColumns(ordBuffer.data(), ox);
					ol.setColumns(ordColumns);

					// Recalculate ordinal fs
					omxCopyMatrix(ordMeans, means);
					omxRemoveElements(ordMeans, ordRemove.data()); 	    // Reduce the row to just ordinal.
					
					EigenVectorAdaptor EordMeans(ordMeans);
					ol.setMean(EordMeans);
				}
			} 
			else if( numIdenticalDefs <= 0 || numIdenticalContinuousRows <= 0 || firstRow) {
				
				/* Reset and Resample rows if necessary. */
				// First Cov and Means (if they've changed)
				if( numIdenticalDefs <= 0 || numIdenticalContinuousMissingness <= 0 || firstRow) {
					if(OMX_DEBUG_ROWS(row)) { mxLog("Beginning to recompute inverse cov for standard models"); }
					
					if(!strcmp(expectation->expType, "MxExpectationGREML")){
						smallMeans = omxGetExpectationComponent(expectation, "means");
						smallCov = omxGetExpectationComponent(expectation, "invcov");
						info = (int) omxGetExpectationComponent(expectation, "cholV_fail_om")->data[0];
						if(info!=0) {
							if (fc) fc->recordIterationError("expected covariance matrix is not "
											 "positive-definite in data row %d",
											 indexVector[row]);
							return TRUE;
						}
						determinant = 0.5 * omxGetExpectationComponent(expectation, "logdetV_om")->data[0];
						if(OMX_DEBUG_ROWS(row)) { mxLog("0.5*log(det(Cov)) is: %3.3f", determinant);}
					}
					else {
						/* Calculate derminant and inverse of Censored continuousCov matrix */
						omxCopyMatrix(smallMeans, means);
						omxRemoveElements(smallMeans, contRemove.data());
						omxCopyMatrix(smallCov, cov);
						omxRemoveRowsAndColumns(smallCov, contRemove.data(), contRemove.data());
						
						if(OMX_DEBUG_ROWS(row)) { 
							omxPrint(smallCov, "Cont Cov to Invert"); 
						}
						
						F77_CALL(dpotrf)(&u, &(smallCov->rows), smallCov->data, &(smallCov->cols), &info);
						
						if(info != 0) {
							if (fc) fc->recordIterationError("Expected covariance matrix for continuous variables "
											 "is not positive-definite in data row %d", indexVector[row]);
							return true;
						}
						// Calculate determinant: squared product of the diagonal of the decomposition
						// For speed, use sum of logs rather than log of product.
						
						determinant = 0.0;
						for(int diag = 0; diag < (smallCov->rows); diag++) {
							determinant += log(fabs(omxMatrixElement(smallCov, diag, diag)));
						}
						// determinant = determinant * determinant;  // Delayed.
						F77_CALL(dpotri)(&u, &(smallCov->rows), smallCov->data, &(smallCov->cols), &info);
					}
					
					if(info != 0) {
						omxRaiseErrorf("Cannot invert expected continuous "
							       "covariance matrix for row %d. Error %d.",
							       indexVector[row], info);
						return true;
					}
				}
				
				// Reset continuous data row (always needed)
				omxCopyMatrix(contRow, smallRow);
				omxRemoveElements(contRow, contRemove.data()); 	// Reduce the row to just continuous.
				F77_CALL(daxpy)(&(contRow->cols), &minusoned, smallMeans->data, &onei, contRow->data, &onei);
				
				/* Calculate Row Likelihood */
				/* Mathematically: (2*pi)^cols * 1/sqrt(determinant(ExpectedCov)) * (dataRow %*% (solve(ExpectedCov)) %*% t(dataRow))^(1/2) */
				//EigenMatrixAdaptor EsmallCov(smallCov);
				//mxPrintMat("smallcov", EsmallCov);
				//omxPrint(contRow, "contRow");
				F77_CALL(dsymv)(&u, &(smallCov->rows), &oned, smallCov->data, &(smallCov->cols), contRow->data, &onei, &zerod, RCX->data, &onei);       // RCX is the continuous-column mahalanobis distance.
				Q = F77_CALL(ddot)(&(contRow->cols), contRow->data, &onei, RCX->data, &onei); //Q is the total mahalanobis distance
				
				if(numOrdinal > 0) { // also check numIdenticalDefs?
					
					// Precalculate Ordinal things that change with continuous changes
					// Reserve: 1) Inverse continuous covariance (smallCov)
					//          2) Columnwise Mahalanobis distance (contCov^-1)%*%(Data - Means) (RCX)
					//          3) Overall Mahalanobis distance (FIML likelihood of data) (Q)
					//Calculate:4) Cont/ord covariance %*% Mahalanobis distance  (halfCov)
					//          5) ordCov <- ordCov - Cont/ord covariance %*% Inverse continuous cov
					
					if(numIdenticalContinuousMissingness <= 0 || firstRow) {
						// Re-sample covariance between ordinal and continuous only if the continuous missingness changes.
						omxCopyMatrix(ordContCov, cov);
						omxRemoveRowsAndColumns(ordContCov, contRemove.data(), ordRemove.data());
						
						// TODO: Make this less of a hack.
						halfCov->rows = smallCov->rows;
						halfCov->cols = ordContCov->cols;
						omxMatrixLeadingLagging(halfCov);
						reduceCov->rows = ordContCov->cols;
						reduceCov->cols = ordContCov->cols;
						omxMatrixLeadingLagging(reduceCov);
						
						F77_CALL(dsymm)(&l, &u, &(smallCov->rows), &(ordContCov->cols), &oned, smallCov->data, &(smallCov->leading), ordContCov->data, &(ordContCov->leading), &zerod, halfCov->data, &(halfCov->leading));          // halfCov is inverse continuous %*% cont/ord covariance
						F77_CALL(dgemm)((ordContCov->minority), (halfCov->majority), &(ordContCov->cols), &(halfCov->cols), &(ordContCov->rows), &oned, ordContCov->data, &(ordContCov->leading), halfCov->data, &(halfCov->leading), &zerod, reduceCov->data, &(reduceCov->leading));      // reduceCov is cont/ord^T %*% (contCov^-1 %*% cont/ord)
					}
					
					if(numIdenticalOrdinalMissingness <= 0 || firstRow) {
						// Means, projected covariance, and Columnwise mahalanobis distance must be recalculated
						//   unless there are no ordinal variables or the continuous variables are identical
						
						// Recalculate Ordinal and Ordinal/Continuous covariance matrices.
						if(OMX_DEBUG_ROWS(row)) {
							mxLog("Resetting Ordinal Covariance Matrix.");
							omxPrint(ordCov, "Was:");
						}
						
						omxCopyMatrix(ordCov, cov);
						if(OMX_DEBUG_ROWS(row)) {
							mxLog("Resetting/Filtering Ordinal Covariance Matrix.");
							omxPrint(ordCov, "Reset to:");
						}
						
						omxRemoveRowsAndColumns(ordCov, ordRemove.data(), ordRemove.data());
						if(OMX_DEBUG_ROWS(row)) {
							mxLog("Resetting/Filtering Ordinal Covariance Matrix.");
							omxPrint(ordCov, "Filtered to:");
						}
						
						// FIXME: This assumes that ordCov and reducCov have the same row/column majority.
						int vlen = reduceCov->rows * reduceCov->cols;
						F77_CALL(daxpy)(&vlen, &minusoned, reduceCov->data, &onei, ordCov->data, &onei); // ordCov <- (ordCov - reduceCov) %*% cont/ord

						EigenMatrixAdaptor EordCov(ordCov);
						ol.setCovariance(EordCov, fc);

						int ox=0;
						for(int j = 0; j < dataColumns.size(); j++) {
							if(ordRemove[j]) continue;         // NA or non-ordinal
							ordBuffer[ox] = j;
							ox += 1;
						}
						Eigen::Map< Eigen::ArrayXi > ordColumns(ordBuffer.data(), ox);
						ol.setColumns(ordColumns);
					}
					
					// Projected means must be recalculated if the continuous variables change at all.
					omxCopyMatrix(ordMeans, means);
					omxRemoveElements(ordMeans, ordRemove.data()); 	    // Reduce the row to just ordinal.
					F77_CALL(dgemv)((smallCov->minority), &(halfCov->rows), &(halfCov->cols), &oned, halfCov->data, &(halfCov->leading), contRow->data, &onei, &oned, ordMeans->data, &onei);                      // ordMeans += halfCov %*% contRow
					EigenVectorAdaptor EordMeans(ordMeans);
					ol.setMean(EordMeans);
				}
				
			} // End of continuous likelihood values calculation
			
			if(numOrdinal <= 0) {       // No Ordinal Vars at all.
			likelihood = 1;
			} 
			else {  
				likelihood = ol.likelihood(indexVector[row]);

				if (likelihood == 0.0) {
					if (fc) fc->recordIterationError("Improper value detected by integration routine "
									 "in data row %d: Most likely the maximum number of "
									 "ordinal variables (20) has been exceeded.  \n"
									 " Also check that expected covariance matrix is not "
									 "positive-definite", indexVector[row]);
					return true;
				}
			}
			
			double rowLikelihood = pow(2 * M_PI, -.5 * numContinuous) * (1.0/exp(determinant)) * exp(-.5 * Q) * likelihood;
			
			if(OMX_DEBUG_ROWS(row)) { 
				mxLog("row[%d] log likelihood det %3.3f + q %3.3f + const %3.3f + ord %3.3f = %3.3g", 
				      indexVector[row], (2.0*determinant), Q, M_LN_2PI * numContinuous, 
				      log(likelihood), -2.0 * log(rowLikelihood));
			}

			omxFIMLAdvanceJointRow(&row, &numIdenticalDefs, 
					       &numIdenticalContinuousMissingness,
					       &numIdenticalOrdinalMissingness, 
					       &numIdenticalContinuousRows,
					       shared_ofo, numDefs, numIdentical);
			recordRow(rowLikelihood);
	}
	return FALSE;
}

bool condOrdByRow::eval()
{
	using Eigen::MatrixXd;
	using Eigen::VectorXd;

	std::vector<bool> isOrdinal(dataColumns.size());

	int numOrdinal=0;
	int numContinuous=0;
	for(int j = 0; j < dataColumns.size(); j++) {
		int var = dataColumns[j];
		isOrdinal[j] = omxDataColumnIsFactor(data, var);
		if (isOrdinal[j]) numOrdinal += 1;
		else numContinuous += 1;
	}

	Eigen::VectorXd cData(numContinuous);
	Eigen::VectorXi iData(numOrdinal);
	Eigen::VectorXi prevOrdData;
	Eigen::VectorXi ordColBuf(numOrdinal);
	std::vector<bool> isMissing(dataColumns.size());

	EigenVectorAdaptor jointMeans(ofo->means);
	EigenMatrixAdaptor jointCov(ofo->cov);

	SimpCholesky< Eigen::MatrixXd >  covDecomp;
	double ordLikelihood = 1.0;

	Eigen::VectorXd contMean;
	Eigen::MatrixXd contCov;
		
	while(row < lastrow) {
		mxLogSetCurrentRow(row);
		int sortedRow = indexVector[row];
		bool numVarsFilled = expectation->loadDefVars(sortedRow);
		if (numVarsFilled || firstRow) {
			omxExpectationCompute(fc, expectation, NULL);
			// automatically fills in jointMeans, jointCov
			covDecomp.compute(jointCov);
			if (covDecomp.info() != Eigen::Success || !(covDecomp.vectorD().array() > 0.0).all()) return true;
		}

		int rowOrdinal = 0;
		int rowContinuous = 0;
		for(int j = 0; j < dataColumns.size(); j++) {
			int var = dataColumns[j];
			if (isOrdinal[j]) {
				int value = omxIntDataElement(data, sortedRow, var);
				isMissing[j] = value == NA_INTEGER;
				if (!isMissing[j]) {
					ordColBuf[rowOrdinal] = j;
					iData[rowOrdinal++] = value;
				}
			} else {
				double value = omxDoubleDataElement(data, sortedRow, var);
				isMissing[j] = std::isnan(value);
				if (!isMissing[j]) cData[rowContinuous++] = value;
			}
		}

		if (thresholdsMat && (numVarsFilled || firstRow)) {
			omxRecompute(thresholdsMat, fc);
			for(int j=0; j < dataColumns.size(); j++) {
				int var = dataColumns[j];
				if (!omxDataColumnIsFactor(data, var)) continue;
				if (!thresholdsIncreasing(thresholdsMat, thresholdCols[j].column,
							  thresholdCols[j].numThresholds, fc)) return true;
			}
		}

		struct subsetOp {
			std::vector<bool> &isOrdinal;
			std::vector<bool> &isMissing;
			bool wantOrdinal;
			subsetOp(std::vector<bool> &_isOrdinal,
				 std::vector<bool> &_isMissing) : isOrdinal(_isOrdinal), isMissing(_isMissing) {};
			// true to include
			bool operator()(int gx) { return !((wantOrdinal ^ isOrdinal[gx]) || isMissing[gx]); };
		} op(isOrdinal, isMissing);

		bool newOrdData = (rowOrdinal && (prevOrdData.size() != iData.size() ||
						  (prevOrdData.array() != iData.array()).any() ||
						  numVarsFilled));
		if (newOrdData) {
			prevOrdData = iData;
			Eigen::VectorXd ordMean;
			Eigen::MatrixXd ordCov;
			op.wantOrdinal = true;
			subsetNormalDist(jointMeans, jointCov, op, rowOrdinal, ordMean, ordCov);
			ol.setCovariance(ordCov, fc);
			Eigen::Map< Eigen::ArrayXi > ordColumns(ordColBuf.data(), rowOrdinal);
			ol.setColumns(ordColumns);
			ol.setMean(ordMean);
			ordLikelihood = ol.likelihood(sortedRow);

			if (rowContinuous) {
				VectorXd truncMean;
				MatrixXd truncCov;
				MatrixXd V11;  //ord
				MatrixXd V12(rowOrdinal, rowContinuous);
				MatrixXd V22;  //cont

				op.wantOrdinal = true;
				subsetCovariance(jointCov, op, rowOrdinal, V11);
				op.wantOrdinal = false;
				upperRightCovariance(jointCov, op, V12);
				subsetNormalDist(jointMeans, jointCov, op, rowContinuous, contMean, V22);

				// skip if ordinal part is the same TODO
				std::vector< omxThresholdColumn > &colInfo = expectation->thresholds;
				EigenMatrixAdaptor tMat(thresholdsMat);
				Eigen::VectorXd uThresh(rowOrdinal);
				Eigen::VectorXd lThresh(rowOrdinal);
				for(int jj=0; jj < rowOrdinal; jj++) {
					int var = dataColumns[ ordColBuf[jj] ];
					if (OMX_DEBUG && !omxDataColumnIsFactor(data, var)) Rf_error("Must be a factor");
					int pick = omxIntDataElement(data, sortedRow, var) - 1;
					if (OMX_DEBUG && (pick < 0 || pick > colInfo[var].numThresholds)) Rf_error("Out of range");
					int tcol = colInfo[var].column;
					if (pick == 0) {
						lThresh[jj] = -std::numeric_limits<double>::infinity();
						uThresh[jj] = (tMat(pick, tcol) - ordMean[jj]);
					} else if (pick == colInfo[var].numThresholds) {
						lThresh[jj] = (tMat(pick-1, tcol) - ordMean[jj]);
						uThresh[jj] = std::numeric_limits<double>::infinity();
					} else {
						lThresh[jj] = (tMat(pick-1, tcol) - ordMean[jj]);
						uThresh[jj] = (tMat(pick, tcol) - ordMean[jj]);
					}
				}

				VectorXd xi;
				MatrixXd U11;
				_mtmvnorm(ordLikelihood, V11, lThresh, uThresh, xi, U11);
				U11 = U11.selfadjointView<Eigen::Upper>();

				MatrixXd invV11 = V11; // cache TODO
				if (InvertSymmetricPosDef(invV11, 'L')) Rf_error("Non-positive definite");
				invV11 = invV11.selfadjointView<Eigen::Lower>();

				// Aitken (1934) "Note on Selection from a Multivariate Normal Population"
				// Or Johnson/Kotz (1972), p.70
				VectorXd mu2 = xi.transpose() * invV11.selfadjointView<Eigen::Lower>() * V12; // factor last terms TODO
				truncMean.derived().resize(dataColumns.size());
				for (int xx=0, x1=0, x2=0; xx < dataColumns.size(); ++xx) {
					if (isOrdinal[xx]) {
						truncMean[xx] = xi[x1++];
					} else {
						truncMean[xx] = mu2[x2] + contMean[x2];
						x2 += 1;
					}
				}
				truncCov.derived().resize(dataColumns.size(), dataColumns.size());
				op.wantOrdinal = true;
				subsetCovarianceStore(truncCov, op, U11);
				op.wantOrdinal = false;
				MatrixXd tmp = U11.selfadjointView<Eigen::Lower>() * (invV11.selfadjointView<Eigen::Lower>() * V12);
				upperRightCovarianceStore(truncCov, op, tmp);
				tmp = (V22 - V12.transpose() * (invV11 -
								invV11.selfadjointView<Eigen::Lower>() * U11 *
								invV11.selfadjointView<Eigen::Lower>()) * V12);
				subsetCovarianceStore(truncCov, op, tmp); // don't need to store it TODO

				truncCov = truncCov.template selfadjointView<Eigen::Upper>();

				subsetNormalDist(truncMean, truncCov, op, rowContinuous, contMean, contCov);
				// Only need continuous subset; remove extra code TODO
				//mxPrintMat("cont mean", contMean);
				//mxPrintMat("cont cov", contCov);
				covDecomp.compute(contCov);
				if (covDecomp.info() != Eigen::Success || !(covDecomp.vectorD().array() > 0.0).all()) return true;
				covDecomp.refreshInverse();
			}
		}

		double contLikelihood = 1.0;
		if (rowContinuous) {
			if (!rowOrdinal && (numVarsFilled || firstRow)) {
				contMean = jointMeans;
				contCov = jointCov;
				covDecomp.compute(contCov);
				if (covDecomp.info() != Eigen::Success || !(covDecomp.vectorD().array() > 0.0).all()) return true;
				covDecomp.refreshInverse();
			}
			Eigen::VectorXd resid = cData - contMean;
			const Eigen::MatrixXd &iV = covDecomp.getInverse();
			double iqf = resid.transpose() * iV.selfadjointView<Eigen::Lower>() * resid;
			double cterm = M_LN_2PI * resid.size();
			double logDet = covDecomp.log_determinant();
			//mxLog("[%d] cont %f %f %f", sortedRow, iqf, cterm, logDet);
			contLikelihood = exp(-0.5 * (iqf + cterm + logDet));
		}

		recordRow(ordLikelihood * contLikelihood);
	}

	return false;
}

