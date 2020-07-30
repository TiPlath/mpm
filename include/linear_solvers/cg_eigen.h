#ifndef MPM_CG_EIGEN_H_
#define MPM_CG_EIGEN_H_

#include <cmath>

#include "factory.h"
#include "solver_base.h"
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

namespace mpm {

//! MPM Eigen CG class
//! \brief Conjugate Gradient solver class using Eigen
template <typename Traits>
class CGEigen : public SolverBase<Traits> {
 public:
  //! Constructor
  //! \param[in] max_iter Maximum number of iterations
  //! \param[in] tolerance Tolerance for solver to achieve convergence
  CGEigen(unsigned max_iter, double tolerance)
      : mpm::SolverBase<Traits>(max_iter, tolerance) {
    //! Logger
    console_ = spdlog::stdout_color_mt("EigenSolver");
  };

  //! Matrix solver with default initial guess
  Eigen::VectorXd solve(const Eigen::SparseMatrix<double>& A,
                        const Eigen::VectorXd& b,
                        std::string solver_type) override;

  //! Return the type of solver
  std::string solver_type() const { return "Eigen"; }

 protected:
  //! Maximum number of iterations
  using SolverBase<Traits>::max_iter_;
  //! Tolerance
  using SolverBase<Traits>::tolerance_;
  //! Logger
  using SolverBase<Traits>::console_;
};
}  // namespace mpm

#include "cg_eigen.tcc"

#endif  // MPM_CG_EIGEN_H_
