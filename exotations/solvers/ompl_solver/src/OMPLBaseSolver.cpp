/*
 * OMPLBaseSolver.cpp
 *
 *  Created on: 22 Feb 2016
 *      Author: yiming
 */

#include "ompl_solver/OMPLBaseSolver.h"

namespace exotica
{
  OMPLBaseSolver::OMPLBaseSolver()
      : timeout_(60.0), finishedSolving_(false)
  {

  }
  OMPLBaseSolver::~OMPLBaseSolver()
  {

  }

  void OMPLBaseSolver::recordData()
  {
    ompl::base::PlannerData data(ompl_simple_setup_->getSpaceInformation());
    ompl_simple_setup_->getPlanner()->getPlannerData(data);
    int cnt =
        prob_->getScenes().begin()->second->getCollisionScene()->stateCheckCnt_;
    prob_->getScenes().begin()->second->getCollisionScene()->stateCheckCnt_ = 0;
  }

  std::string OMPLBaseSolver::getAlgorithm()
  {
    return algorithm_;
  }

  double OMPLBaseSolver::getPlanningTime()
  {
    return planning_time_.toSec();
  }

  void OMPLBaseSolver::preSolve()
  {
    // clear previously computed solutions
    ompl_simple_setup_->getProblemDefinition()->clearSolutionPaths();
    const ob::PlannerPtr planner = ompl_simple_setup_->getPlanner();
    if (planner) planner->clear();
    ompl_simple_setup_->getSpaceInformation()->getMotionValidator()->resetMotionCounter();
    ompl_simple_setup_->getPlanner()->setProblemDefinition(
        ompl_simple_setup_->getProblemDefinition());
  }

  void OMPLBaseSolver::postSolve()
  {
    ompl_simple_setup_->clearStartStates();
    int v =
        ompl_simple_setup_->getSpaceInformation()->getMotionValidator()->getValidMotionCount();
    int iv =
        ompl_simple_setup_->getSpaceInformation()->getMotionValidator()->getInvalidMotionCount();
    logDebug("There were %d valid motions and %d invalid motions.", v, iv);

    if (ompl_simple_setup_->getProblemDefinition()->hasApproximateSolution())
      logWarn("Computed solution is approximate");
  }

  void OMPLBaseSolver::registerTerminationCondition(
      const ob::PlannerTerminationCondition &ptc)
  {
    boost::mutex::scoped_lock slock(ptc_lock_);
    ptc_ = &ptc;
  }

  void OMPLBaseSolver::unregisterTerminationCondition()
  {
    boost::mutex::scoped_lock slock(ptc_lock_);
    ptc_ = NULL;
  }
}
