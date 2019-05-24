//
// Copyright (c) 2019, University of Edinburgh
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of  nor the names of its contributors may be used to
//    endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#include <exotica_ilqr_solver/ilqr_solver.h>

REGISTER_MOTIONSOLVER_TYPE("ILQRSolver", exotica::ILQRSolver)

namespace exotica
{
ILQRSolver::ILQRSolver() = default;
ILQRSolver::~ILQRSolver() = default;

void ILQRSolver::SpecifyProblem(PlanningProblemPtr pointer)
{
    if (pointer->type() != "exotica::DynamicTimeIndexedShootingProblem")
    {
        ThrowNamed("This ILQRSolver can't solve problem of type '" << pointer->type() << "'!");
    }
    MotionSolver::SpecifyProblem(pointer);
    prob_ = std::static_pointer_cast<DynamicTimeIndexedShootingProblem>(pointer);
    dynamics_solver_ = prob_->GetScene()->GetDynamicsSolver();
    if (debug_) HIGHLIGHT_NAMED("ILQRSolver", "initialized");
}

void ILQRSolver::BackwardPass()
{
    constexpr double min_clamp_ = -1e10;
    constexpr double max_clamp_ = 1e10;
    int T = prob_->get_T();

    const Eigen::MatrixXd Qf = prob_->get_Qf(), R = prob_->get_R();
    const Eigen::MatrixXd X_star = prob_->get_X_star();

    // eq. 18
    vk_gains_[T - 1] = Qf * dynamics_solver_->StateDelta(prob_->get_X(T - 1), X_star.col(T - 1));
    vk_gains_[T - 1] = vk_gains_[T - 1].unaryExpr([min_clamp_, max_clamp_](double x) -> double {
        return std::min(std::max(x, min_clamp_), max_clamp_);
    });

    Eigen::MatrixXd Sk = Qf;
    Sk = Sk.unaryExpr([min_clamp_, max_clamp_](double x) -> double {
        return std::min(std::max(x, min_clamp_), max_clamp_);
    });

    for (int i = T - 2; i > 0; i--)
    {
        Eigen::VectorXd x = prob_->get_X(i), u = prob_->get_U(i);
        Eigen::MatrixXd Ak = dynamics_solver_->fx(x, u), Bk = dynamics_solver_->fu(x, u),
                        Q = prob_->get_Q(i);

        // eq. 13-16
        Ak = Ak * dynamics_solver_->get_dt() + Eigen::MatrixXd::Identity(Ak.rows(), Ak.cols());
        Bk = Bk * dynamics_solver_->get_dt();
        // this inverse is common for all factors
        const Eigen::MatrixXd _inv =
            (Eigen::MatrixXd::Identity(R.rows(), R.cols()) * 1e-5 + R + Bk.transpose() * Sk * Bk).inverse();

        Kv_gains_[i] = _inv * Bk.transpose();
        K_gains_[i] = _inv * Bk.transpose() * Sk * Ak;
        Ku_gains_[i] = _inv * R;
        Sk = Ak.transpose() * Sk * (Ak - Bk * K_gains_[i]) + Q;

        vk_gains_[i] = ((Ak - Bk * K_gains_[i]).transpose() * vk_gains_[i + 1]) -
                       (K_gains_[i].transpose() * R * u) + (Q * x);

        // fix for large values
        Sk = Sk.unaryExpr([min_clamp_, max_clamp_](double x) -> double {
            return std::min(std::max(x, min_clamp_), max_clamp_);
        });
        vk_gains_[i] = vk_gains_[i].unaryExpr([min_clamp_, max_clamp_](double x) -> double {
            return std::min(std::max(x, min_clamp_), max_clamp_);
        });
    }
}

double ILQRSolver::ForwardPass(double alpha, Eigen::MatrixXdRef ref_trajectory)
{
    double cost = 0;
    int T = prob_->get_T();
    const Eigen::VectorXd control_limits = dynamics_solver_->get_control_limits();

    DynamicsSolverPtr dynamics_solver_ = prob_->GetScene()->GetDynamicsSolver();
    for (int i = 0; i < T - 1; ++i)
    {
        Eigen::VectorXd u = prob_->get_U(i);
        // eq. 12
        Eigen::VectorXd delta_uk = -Ku_gains_[i] * u - Kv_gains_[i] * vk_gains_[i + 1] -
                                   K_gains_[i] * dynamics_solver_->StateDelta(prob_->get_X(i), ref_trajectory.col(i));

        u = u + alpha * delta_uk;
        // clamp controls
        if (control_limits.size() == u.size())
            u = u.cwiseMax(-control_limits).cwiseMin(control_limits);
        else if (control_limits.size() == 1)
            u = u.unaryExpr([&control_limits](double x) -> double {
                return std::min(std::max(x, -control_limits(0)), control_limits(0));
            });

        prob_->Update(u, i);
        cost += prob_->GetControlCost(i) + prob_->GetStateCost(i);
    }

    // add terminal cost
    cost += prob_->GetStateCost(T - 1);
    return cost;
}

void ILQRSolver::Solve(Eigen::MatrixXd& solution)
{
    const int T = prob_->get_T();
    const int NX = prob_->get_num_positions();
    const int NU = prob_->get_num_controls();
    const int N = NX + prob_->get_num_velocities();

    prob_->ResetCostEvolution(GetNumberOfMaxIterations() + 1);
    if (!prob_) ThrowNamed("Solver has not been initialized!");

    // initialize Gain matrices
    K_gains_.assign(T, Eigen::MatrixXd(NU, N));
    Ku_gains_.assign(T, Eigen::MatrixXd(NU, NU));
    Kv_gains_.assign(T, Eigen::MatrixXd(NU, N));
    vk_gains_.assign(T, Eigen::MatrixXd(N, 1));

    // all of the below are not pointers, since we want to copy over
    //  solutions across iterations
    Eigen::MatrixXd best_U, best_X, global_best_U;
    solution.resize(T, NU);

    if (debug_) HIGHLIGHT_NAMED("ILQRSolver", "Running ILQR solver for max " << parameters_.MaxIterations << " iterations");

    double last_cost = 0, global_best_cost = 0;
    int last_best_iteration = 0;

    for (int iteration = 0; iteration < GetNumberOfMaxIterations(); ++iteration)
    {
        // Backwards pass computes the gains
        BackwardPass();
        if (debug_) HIGHLIGHT_NAMED("ILQRSolver", "Backward pass complete");

        // forward pass to compute new control trajectory
        // TODO: Configure line-search space from xml
        // TODO (Wolf): What you are doing is a forward line-search when we may try a
        //    backtracking line search for a performance improvement later - this just as an aside.
        const Eigen::VectorXd alpha_space = Eigen::VectorXd::LinSpaced(10, 0.1, 1.0);
        double best_cost = 0, best_alpha = 0;

        Eigen::MatrixXd ref_trajectory = prob_->get_X();
        // perform a linear search to find the best rate
        for (int ai = 0; ai < alpha_space.rows(); ++ai)
        {
            double alpha = alpha_space(ai);
            double cost = ForwardPass(alpha, ref_trajectory);

            if (ai == 0 || cost < best_cost)
            {
                best_cost = cost;
                best_X = prob_->get_X();
                best_U = prob_->get_U();
                best_alpha = alpha;
            }
        }

        if (debug_)
        {
            HIGHLIGHT_NAMED("ILQRSolver", "Forward pass complete with cost: " << best_cost << " and alpha " << best_alpha);
            HIGHLIGHT_NAMED("ILQRSolver", "Final state: " << best_X.col(T - 1).transpose());
        }

        // copy solutions for next iteration
        if (iteration == 0 || global_best_cost > best_cost)
        {
            global_best_cost = best_cost;
            last_best_iteration = iteration;
            global_best_U = best_U;
        }

        if (iteration - last_best_iteration > parameters_.FunctionTolerancePatience)
        {
            if (debug_) HIGHLIGHT_NAMED("ILQRSolver", "Early stopping criterion reached.");
            break;
        }

        if (abs(best_cost - last_cost) < parameters_.FunctionTolerance)
        {
            if (debug_) HIGHLIGHT_NAMED("ILQRSolver", "Function tolerance reached.");
            break;
        }

        if (iteration == parameters_.MaxIterations - 1 && debug_) HIGHLIGHT_NAMED("ILQRSolver", "Max iterations reached.");

        last_cost = best_cost;
        for (int i = 0; i < T - 1; ++i)
            prob_->Update(best_U.col(i), i);
        prob_->SetCostEvolution(iteration, best_cost);
    }

    // store the best solution found over all iterations
    for (int i = 0; i < T - 1; ++i)
    {
        solution.row(i) = global_best_U.col(i);
        prob_->Update(global_best_U.col(i), i);
    }
}
}  // namespace exotica
