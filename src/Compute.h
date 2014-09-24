/*
 *  Copyright 2013 The OpenMx Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef _OMX_COMPUTE_H_
#define _OMX_COMPUTE_H_

#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>
#include "omxDefines.h"
#include "Eigen/SparseCore"
#include "glue.h"

// See R/MxRunHelperFunctions.R npsolMessages
// Also see the NPSOL manual, section 6 "Error Indicators and Warnings"
// These are ordered from good to bad so we can use max() on a set
// of inform results to obtain a bound on convergence status.
typedef int ComputeInform;
#define INFORM_UNINITIALIZED -1
#define INFORM_CONVERGED_OPTIMUM 0
#define INFORM_UNCONVERGED_OPTIMUM 1
	// The final iterate satisfies the optimality conditions to the accuracy requested,
	// but the sequence of iterates has not yet converged.
	// Optimizer was terminated because no further improvement
	// could be made in the merit function (Mx status GREEN).
#define INFORM_LINEAR_CONSTRAINTS_INFEASIBLE 2
	// The linear constraints and bounds could not be satisfied.
	// The problem has no feasible solution.
#define INFORM_NONLINEAR_CONSTRAINTS_INFEASIBLE 3
	// The nonlinear constraints and bounds could not be satisfied.
	// The problem may have no feasible solution.
#define INFORM_ITERATION_LIMIT 4
	// The major iteration limit was reached (Mx status BLUE).
#define INFORM_NOT_AT_OPTIMUM 6
	// The model does not satisfy the first-order optimality conditions (i.e. gradient close to zero)
	// to the required accuracy, and no improved point for the
	// merit function could be found during the final linesearch (Mx status RED)
#define INFORM_BAD_DERIVATIVES 7
	// The function derivates returned by funcon or funobj
	// appear to be incorrect.
#define INFORM_INVALID_PARAM 9
	// An input parameter was invalid'

enum ComputeInfoMethod {
	INFO_METHOD_DEFAULT,
	INFO_METHOD_HESSIAN,
	INFO_METHOD_SANDWICH,
	INFO_METHOD_BREAD,
	INFO_METHOD_MEAT
};

struct HessianBlock {
	std::vector<int> vars;  // global freeVar ID in order
	Eigen::MatrixXd mat;    // vars * vars, only upper triangle referenced
	Eigen::MatrixXd mmat;   // including subblocks
	Eigen::MatrixXd imat;
	std::vector< HessianBlock* > subBlocks;
	bool merge;
	int useId;

	HessianBlock() : merge(false), useId(0) {}
	HessianBlock *clone();
	void addSubBlocks();
	int estNonZero() const;
};

// The idea of FitContext is to eventually enable fitting from
// multiple starting values in parallel.

class FitContext {
	static omxFitFunction *RFitFunction;

	FitContext *parent;

	std::vector<HessianBlock*> allBlocks;
	std::vector<HessianBlock*> mergeBlocks;
	std::vector<HessianBlock*> blockByVar;

	bool haveSparseHess;
	Eigen::SparseMatrix<double> sparseHess;
	bool haveSparseIHess;
	Eigen::SparseMatrix<double> sparseIHess;
	int estNonZero;
	int minBlockSize;
	int maxBlockSize;

	bool haveDenseHess;
	Eigen::MatrixXd hess;
	bool haveDenseIHess;
	Eigen::MatrixXd ihess;

	std::string IterationError;

	void init();
	void analyzeHessian();
	void analyzeHessianBlock(HessianBlock *hb);
	void testMerge();

 public:
	FreeVarGroup *varGroup;
	omxState *state;
	size_t numParam;               // change to int type TODO
	std::vector<int> mapToParent;
	double mac;
	double fit;
	double *est;
	std::vector<const char*> flavor;
	Eigen::VectorXd grad;
	int infoDefinite;
	double infoCondNum;
	double *stderrs;   // plural to distinguish from stdio's stderr
	enum ComputeInfoMethod infoMethod;
	double *infoA; // sandwich, the bread
	double *infoB; // sandwich, the meat
	int iterations;
	ComputeInform inform;
	int wanted;
	std::vector< class FitContext* > childList;

	FitContext(omxState *_state, std::vector<double> &startingValues);
	FitContext(FitContext *parent, FreeVarGroup *group);
	void createChildren();
	void allocStderrs();
	void copyParamToModel();
	void copyParamToModelClean();
	double *take(int want);
	omxMatrix *lookupDuplicate(omxMatrix* element);
	void maybeCopyParamToModel(omxState* os);
	void updateParent();
	void updateParentAndFree();
	template <typename T> void moveInsideBounds(std::vector<T> &prevEst);
	void log(int what);
	~FitContext();
	
	// deriv related
	void clearHessian();
	void negateHessian();
	void queue(HessianBlock *hb);
	void refreshDenseHess();
	void refreshDenseIHess();
	void refreshSparseHess();
	bool refreshSparseIHess(); // NOTE: produces an ihess with filtered eigenvalues
	Eigen::VectorXd ihessGradProd();
	double *getDenseHessUninitialized();
	double *getDenseIHessUninitialized();
	double *getDenseHessianish();  // either a Hessian or inverse Hessian, remove TODO
	void copyDenseHess(double *dest);
	void copyDenseIHess(double *dest);
	Eigen::VectorXd ihessDiag();
	void preInfo();
	void postInfo();
	void resetIterationError() { IterationError.clear(); }
	void recordIterationError(const char* msg, ...) __attribute__((format (printf, 2, 3)));

	std::string getIterationError();

	static void cacheFreeVarDependencies();
	static void setRFitFunction(omxFitFunction *rff);
};

typedef std::vector< std::pair<int, MxRList*> > LocalComputeResult;

class omxCompute {
	int computeId;
 protected:
        virtual void reportResults(FitContext *fc, MxRList *slots, MxRList *glob) {};
	void collectResultsHelper(FitContext *fc, std::vector< omxCompute* > &clist,
				  LocalComputeResult *lcr, MxRList *out);
	static enum ComputeInfoMethod stringToInfoMethod(const char *iMethod);
 public:
	const char *name;
	FreeVarGroup *varGroup;
	omxCompute();
        virtual void initFromFrontend(omxState *, SEXP rObj);
        void compute(FitContext *fc);
        virtual void computeImpl(FitContext *fc) {}
	virtual void collectResults(FitContext *fc, LocalComputeResult *lcr, MxRList *out);
        virtual ~omxCompute();
};

omxCompute *omxNewCompute(omxState* os, const char *type);

omxCompute *newComputeGradientDescent();
omxCompute *newComputeNumericDeriv();
omxCompute *newComputeNewtonRaphson();
omxCompute *newComputeConfidenceInterval();

void omxApproxInvertPosDefTriangular(int dim, double *hess, double *ihess, double *stress);
void omxApproxInvertPackedPosDefTriangular(int dim, int *mask, double *packedHess, double *stress);
SEXP sparseInvert_wrapper(SEXP mat);

#endif
