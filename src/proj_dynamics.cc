#include "proj_dynamics.h"

#include <fstream>
#include <iostream>
#include <Eigen/Dense>
#include <unsupported/Eigen/MatrixFunctions>
#include <zjucad/matrix/io.h>

#include "def.h"
#include "energy.h"
#include "geom_util.h"
#include "config.h"
#include "vtk.h"
#include "optimizer.h"

using namespace std;
using namespace Eigen;
using namespace zjucad::matrix;

namespace bigbang {

static Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> solver_;

proj_dyn_spring_solver::proj_dyn_spring_solver(const mati_t &tris, const matd_t &nods)
  : tris_(tris), nods_(nods), dim_(nods.size()) {
  get_edge_elem(tris_, edges_);
  get_diam_elem(tris_, diams_);
}

int proj_dyn_spring_solver::initialize(const proj_dyn_args &args) {
  cout << "[info] init the projective dynamics solver";
  args_ = args;

  impebf_.resize(6);
  impebf_[0] = make_shared<momentum_potential_imp_euler>(tris_, nods_, args_.rho, args_.h);
  switch ( args_.method ) {
    case 0: impebf_[1] = make_shared<fast_mass_spring>(edges_, nods_, args_.ws); break;
    case 1: impebf_[1] = make_shared<fast_mass_spring>(edges_, nods_, args_.ws); break;
    case 2: impebf_[1] = make_shared<modified_fms_energy>(edges_, nods_, args_.ws); break;
    case 3: impebf_[1] = make_shared<fast_mass_spring>(edges_, nods_, args_.ws); break;
    case 4: impebf_[1] = make_shared<fast_mass_spring>(edges_, nods_, args_.ws); break;
    case 5: impebf_[1] = make_shared<fast_mass_spring>(edges_, nods_, args_.ws); break;
    default: break;
  }
  impebf_[2] = make_shared<positional_potential>(nods_, args_.wp);
  impebf_[3] = make_shared<gravitational_potential>(tris_, nods_, args_.rho, args_.wg);
  impebf_[4] = make_shared<ext_force_energy>(nods_, 1e0);
  impebf_[5] = make_shared<isometric_bending>(diams_, nods_, args_.wb);

  try {
    impE_ = make_shared<energy_t<double>>(impebf_);
  } catch ( exception &e ) {
    cerr << "[exception] " << e.what() << endl;
    exit(EXIT_FAILURE);
  }
  cout << "......done\n";
  return 0;
}

int proj_dyn_spring_solver::pin_down_vert(const size_t id, const double *pos) {
  return dynamic_pointer_cast<positional_potential>(impebf_[2])->Pin(id, pos);
}

int proj_dyn_spring_solver::release_vert(const size_t id) {
  return dynamic_pointer_cast<positional_potential>(impebf_[2])->Release(id);
}

int proj_dyn_spring_solver::apply_force(const size_t id, const double *f) {
  return dynamic_pointer_cast<ext_force_energy>(impebf_[4])->ApplyForce(id, f);
}

int proj_dyn_spring_solver::remove_force(const size_t id) {
  return dynamic_pointer_cast<ext_force_energy>(impebf_[4])->RemoveForce(id);
}

int proj_dyn_spring_solver::precompute() {
  cout << "[info] solver is doing precomputation";
  ASSERT(dim_ == impE_->Nx());
  vector<Triplet<double>> trips;
  impE_->Hes(nullptr, &trips);
  LHS_.resize(dim_, dim_);
  LHS_.reserve(trips.size());
  LHS_.setFromTriplets(trips.begin(), trips.end());
  solver_.compute(LHS_);
  ASSERT(solver_.info() == Success);
  cout << "......done\n";
  return 0;
}

int proj_dyn_spring_solver::advance(double *x) const {
  int rtn = 0;
  switch ( args_.method ) {
    case 0: rtn = advance_alpha(x); break;
    case 1: rtn = advance_beta(x); break;
    case 2: rtn = advance_gamma(x); break;
    case 3: rtn = advance_delta(x); break;
    case 4: rtn = advance_epsilon(x); break;
    case 5: rtn = advance_zeta(x); break;
    default: return __LINE__;
  }
  return rtn;
}

int proj_dyn_spring_solver::advance_alpha(double *x) const { /// @brief Direct
  ASSERT(args_.method == 0);
  Map<VectorXd> X(x, dim_);
  VectorXd xstar = X;
  const auto fms = dynamic_pointer_cast<fast_mass_spring>(impebf_[1]);
  // iterate solve
  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
    if ( iter % 1000 == 0 ) {
      double value = 0;
      impE_->Val(&xstar[0], &value);
      cout << "\t@iter " << iter << " energy value: " << value << endl;
    }
    fms->LocalSolve(&xstar[0]);
    VectorXd jac = VectorXd::Zero(dim_); {
      impE_->Gra(&xstar[0], &jac[0]);
      jac *= -1;
    }
    double curr_jac_norm = jac.norm();
    if ( curr_jac_norm <= args_.eps ) {
      cout << "\t@CONVERGED after " << iter << " iterations\n";
      break;
    }
    if ( iter % 1000 == 0 ) {
      cout << "\t@iter " << iter << " error: " << jac.norm() << endl;
    }
    xstar += solver_.solve(jac);
  }
  // update configuration
  dynamic_pointer_cast<momentum_potential>(impebf_[0])->Update(&xstar[0]);
  X = xstar;
  return 0;
}

int proj_dyn_spring_solver::advance_zeta(double *x) const { /// @brief Direct+Chebyshev
  ASSERT(args_.method == 5);
  Map<VectorXd> X(x, dim_);
  VectorXd xstar = X, prev_xstar = xstar, curr_xstar(dim_), dx(dim_);
  const auto fms = dynamic_pointer_cast<fast_mass_spring>(impebf_[1]);
  const size_t S = 10;
  const double rho = 0.9992, gamma = 0.75;
  // iterative solve
  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
    if ( iter % 1000 == 0 ) {
      double value = 0;
      impE_->Val(&xstar[0], &value);
      cout << "\t@iter " << iter << " energy value: " << value << endl;
    }
    fms->LocalSolve(&xstar[0]);
    VectorXd jac = VectorXd::Zero(dim_); {
      impE_->Gra(&xstar[0], &jac[0]);
      jac *= -1;
    }
    double curr_jac_norm = jac.norm();
    if ( curr_jac_norm <= args_.eps ) {
      cout << "\t@CONVERGED after " << iter << " iterations\n";
      break;
    }
    if ( iter % 1000 == 0 ) {
      cout << "\t@iter " << iter << " error: " << jac.norm() << endl;
    }
    dx = solver_.solve(jac);
    double omega;
    if ( iter < S ) // delay the chebyshev iteration
      omega = 1.0;
    else if ( iter == S )
      omega = 2.0/(2.0-rho*rho);
    else
      omega = 4.0/(4.0-rho*rho*omega);
    curr_xstar = xstar;
    xstar = omega*(gamma*dx+curr_xstar-prev_xstar)+prev_xstar;
    prev_xstar = curr_xstar;
  }
  dynamic_pointer_cast<momentum_potential>(impebf_[0])->Update(&xstar[0]);
  X = xstar;
  return 0;
}

int proj_dyn_spring_solver::advance_epsilon(double *x) const { /// @brief Jacobi+Chebyshev
  ASSERT(args_.method == 4);
  Map<VectorXd> X(x, dim_);
  VectorXd xstar = X, prev_xstar = xstar, curr_xstar(dim_), dx(dim_);
  const auto fms = dynamic_pointer_cast<fast_mass_spring>(impebf_[1]);
  const size_t S = 10;
  const double rho = 0.9992, gamma = 0.75;
  // iterative solve
  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
    if ( iter % 1000 == 0 ) {
      double value = 0;
      impE_->Val(&xstar[0], &value);
      cout << "\t@iter " << iter << " energy value: " << value << endl;
    }
    fms->LocalSolve(&xstar[0]);
    VectorXd jac = VectorXd::Zero(dim_); {
      impE_->Gra(&xstar[0], &jac[0]);
      jac *= -1;
    }
    double curr_jac_norm = jac.norm();
    if ( curr_jac_norm <= args_.eps ) {
      cout << "\t@CONVERGED after " << iter << " iterations\n";
      break;
    }
    if ( iter % 1000 == 0 ) {
      cout << "\t@iter " << iter << " error: " << jac.norm() << endl;
    }
    dx.setZero();
    apply_jacobi(LHS_, jac, dx);
    double omega;
    if ( iter < S ) // delay the chebyshev iteration
      omega = 1.0;
    else if ( iter == S )
      omega = 2.0/(2.0-rho*rho);
    else
      omega = 4.0/(4.0-rho*rho*omega);
    curr_xstar = xstar;
    xstar = omega*(gamma*dx+curr_xstar-prev_xstar)+prev_xstar;
    prev_xstar = curr_xstar;
  }
  dynamic_pointer_cast<momentum_potential>(impebf_[0])->Update(&xstar[0]);
  X = xstar;
  return 0;
}

int proj_dyn_spring_solver::advance_beta(double *x) const { /// @brief Kovalsky15
  ASSERT(args_.method == 1);
  return __LINE__;
//  Map<VectorXd> X(x, dim_);
//  auto fms = dynamic_pointer_cast<fast_mass_spring>(impebf_[1]);
//  const size_t fdim = fms->aux_dim();

//  VectorXd xstar = X, jac(dim_), eta(dim_);
//  VectorXd z(fdim), n(fdim);
//  Map<const VectorXd> Pz(fms->get_aux_var(), fdim);
//  double d0 = 0;
//  const SparseMatrix<double>& S = fms->get_df_mat();
//  VectorXd next_step(fdim);

//  // iterative solve
//  VectorXd unkown(dim_+1), inve(dim_), invb(dim_);
//  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
//    if ( iter % 100 == 0 ) {
//      double value = 0;
//      impE_->Val(&xstar[0], &value);
//      cout << "\t@iter " << iter << " energy value: " << value << endl;
//    }
//    // local solve
//    z = S*xstar;
//    fms->LocalSolve(&xstar[0]);
//    n = z-Pz;
//    if ( iter % 100 == 0 ) {
//      cout << "\t@iter " << iter << " normal: " << n.norm() << endl << endl;
//    }
//    // global solve
//    jac.setZero(); {
//      impE_->Gra(&xstar[0], &jac[0]);
//      jac *= -1;
//    }
//    eta = S.transpose()*n;
//    d0 = n.dot(Pz-z);

//    invb = solver_.solve(jac);
//    ASSERT(solver_.info() == Success);
//    inve = solver_.solve(eta);
//    ASSERT(solver_.info() == Success);

//    if ( n.norm() < args_.eps ) {
//      unkown.head(dim_) = invb;
//    } else {
//      unkown[unkown.size()-1] = (-d0+eta.dot(invb))/eta.dot(inve);
//      unkown.head(dim_) = invb-unkown[unkown.size()-1]*inve;
//    }
//    double xstar_norm = xstar.norm();
//    xstar += unkown.head(dim_);
//    next_step = S*xstar-Pz;
//    if ( iter < 5 ) {
//      cout << "\t@prev step size: " << n.norm() << endl;
//      cout << "\t@post step size: " << next_step.norm() << endl;
//      cout << "\t@dx norm: " << unkown.head(dim_).norm() << endl;
//      cout << "\t@turning angle: " << acos(n.dot(next_step)/(n.norm()*next_step.norm()))/M_PI*180 << "\n\n";
//    }
//    if ( unkown.head(dim_).norm() <= args_.eps*xstar_norm ) {
//      cout << "[info] converged after " << iter+1 << " iterations\n";
//      break;
//    }
//  }
//  dynamic_pointer_cast<momentum_potential>(impebf_[0])->Update(&xstar[0]);
//  X = xstar;
//  return 0;
}

int proj_dyn_spring_solver::advance_gamma(double *x) const { /// @brief modified, fail
  ASSERT(args_.method == 2);
  return __LINE__;
//  Map<VectorXd> X(x, dim_);
//  VectorXd xstar = X;
//  const auto fms = dynamic_pointer_cast<modified_fms_energy>(impebf_[1]);
//  VectorXd prev_step, next_step;
//  const static SparseMatrix<double>& S = fms->get_df_mat();
//  // iterate solve
//  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
//    if ( iter % 100 == 0 ) {
//      double value = 0;
//      impE_->Val(&xstar[0], &value);
//      cout << "\t@iter " << iter << " energy value: " << value << endl;
//    }
//    // local step for constraint projection
//    fms->LocalSolve(&xstar[0]);
//    prev_step = S*xstar-Map<const VectorXd>(fms->get_aux_var(), fms->aux_dim());
//    // global step for compatible position
//    VectorXd jac = VectorXd::Zero(dim_); {
//      impE_->Gra(&xstar[0], &jac[0]);
//    }
//    VectorXd dx = -solver_.solve(jac);
//    double xstar_norm = xstar.norm();
//    xstar += dx;
//    next_step = S*xstar-Map<const VectorXd>(fms->get_aux_var(), fms->aux_dim());
//    if ( iter < 5 ) {
//      cout << "\t@prev step size: " << prev_step.norm() << endl;
//      cout << "\t@post step size: " << next_step.norm() << endl;
//      cout << "\t@dx norm: " << dx.norm() << endl;
//      cout << "\t@turning angle: " << acos(prev_step.dot(next_step)/(prev_step.norm()*next_step.norm()))/M_PI*180 << endl << endl;
//    }
//    if ( dx.norm() <= args_.eps*xstar_norm ) {
//      cout << "[info] converged after " << iter+1 << " iterations\n";
//      break;
//    }
//  }
//  // update configuration
//  dynamic_pointer_cast<momentum_potential>(impebf_[0])->Update(&xstar[0]);
//  X = xstar;
//  return 0;
}

static std::vector<Eigen::Vector3d> sphere_pts;
static Eigen::Vector3d d0;
int proj_dyn_spring_solver::advance_delta(double *x) const { /// @brief TODO
  ASSERT(args_.method == 3);
  Map<VectorXd> X(x, dim_);
  VectorXd xstar = X;
  const auto fms = dynamic_pointer_cast<fast_mass_spring>(impebf_[1]);
  Map<const VectorXd> dcurr(fms->get_aux_var(), fms->aux_dim());
  VectorXd dprev = dcurr;
  // iterate solve
  sphere_pts.clear();
  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
    if ( iter % 1000 == 0 ) {
      double value = 0;
      impE_->Val(&xstar[0], &value);
      cout << "\t@iter " << iter << " energy value: " << value << endl;
    }
    // local step for constraint projection
    fms->LocalSolve(&xstar[0]);

      static size_t cnt = 0;
      const size_t eid = 1000;
      if ( cnt++ == 0 ) {
        d0 = Vector3d(fms->get_aux_var()+3*eid);
      }
      Vector3d edge(fms->get_aux_var()+3*eid);
      sphere_pts.push_back(edge);

    // global step for compatible position
    VectorXd jac = VectorXd::Zero(dim_); {
      impE_->Gra(&xstar[0], &jac[0]);
    }
    VectorXd dx = -solver_.solve(jac);
    double xstar_norm = xstar.norm();
    xstar += dx;
    if ( dx.norm() <= args_.eps*xstar_norm ) {
      cout << "[info] converged after " << iter+1 << " iterations\n";
      break;
    }
  }
  // update configuration
  dynamic_pointer_cast<momentum_potential>(impebf_[0])->Update(&xstar[0]);
  X = xstar;
  return 0;
}

int proj_dyn_spring_solver::vis_rot(const char *filename) const {
  if ( args_.method != 3 )
    return __LINE__;
  cout << "[info] visualize the rotation in log space\n";
  vector<double> pts;
  for (size_t i = 0; i < sphere_pts.size(); ++i) {
    Vector3d axis = (d0.cross(sphere_pts[i]));
    if ( axis.norm() < 1e-16 ) {
      pts.push_back(0.0);
      pts.push_back(0.0);
      pts.push_back(0.0);
      continue;
    }
    axis /= axis.norm();
    double angle = acos(d0.dot(sphere_pts[i])/(d0.norm()*sphere_pts[i].norm()));
    axis *= angle;
    pts.push_back(axis[0]);
    pts.push_back(axis[1]);
    pts.push_back(axis[2]);
  }
  ofstream os(filename);
  mati_t p = colon(0, sphere_pts.size()-1);
  point2vtk(os, &pts[0], pts.size()/3, p.begin(), p.size());
  return 0;
}

int proj_dyn_spring_solver::draw_trajectory(const char *filename) const {
  if ( args_.method != 3 )
    return __LINE__;
  cout << "[info] draw the trajectory of spring direction\n";
  vector<double> pts;
  pts.push_back(0.0);
  pts.push_back(0.0);
  pts.push_back(0.0);
  for (size_t i = 0; i < sphere_pts.size(); ++i) {
    pts.push_back(sphere_pts[i].x());
    pts.push_back(sphere_pts[i].y());
    pts.push_back(sphere_pts[i].z());
  }
  ofstream os(filename);
  mati_t p = colon(0, pts.size()/3-1);
  point2vtk(os, &pts[0], pts.size()/3, p.begin(), p.size());
  return 0;
}
//====================== tetrahedral projective solver =========================
proj_dyn_tet_solver::proj_dyn_tet_solver(const mati_t &tets, const matd_t &nods)
  : tets_(tets), nods_(nods), dim_(nods.size()) {}

int proj_dyn_tet_solver::initialize(const proj_dyn_args &args) {
  cout << "[info] init the projective dynamics solver...";
  args_ = args;

  ebf_.resize(5);
  ebf_[0] = make_shared<momentum_potential_imp_euler>(tets_, nods_, args_.rho, args_.h);
  ebf_[1] = make_shared<tet_arap_energy>(tets_, nods_, args_.ws);
  ebf_[2] = make_shared<positional_potential>(nods_, args_.wp);
  ebf_[3] = make_shared<gravitational_potential>(tets_, nods_, args_.rho, args_.wg);
  ebf_[4] = make_shared<ext_force_energy>(nods_, 1e0);

  try {
    energy_ = make_shared<energy_t<double>>(ebf_);
  } catch ( exception &e ) {
    cerr << "[exception] " << e.what() << endl;
    exit(EXIT_FAILURE);
  }
  cout << "done\n";
  return 0;
}

int proj_dyn_tet_solver::pin_down_vert(const size_t id, const double *pos) {
  return dynamic_pointer_cast<positional_potential>(ebf_[2])->Pin(id, pos);
}

int proj_dyn_tet_solver::release_vert(const size_t id) {
  return dynamic_pointer_cast<positional_potential>(ebf_[2])->Release(id);
}

int proj_dyn_tet_solver::apply_force(const size_t id, const double *f) {
  return dynamic_pointer_cast<ext_force_energy>(ebf_[4])->ApplyForce(id, f);
}

int proj_dyn_tet_solver::remove_force(const size_t id) {
  return dynamic_pointer_cast<ext_force_energy>(ebf_[4])->RemoveForce(id);
}

int proj_dyn_tet_solver::precompute() {
  cout << "[info] solver is doing precomputation";
  ASSERT(dim_ == energy_->Nx());
  vector<Triplet<double>> trips;
  energy_->Hes(nullptr, &trips);
  LHS_.resize(dim_, dim_);
  LHS_.reserve(trips.size());
  LHS_.setFromTriplets(trips.begin(), trips.end());
  solver_.compute(LHS_);
  ASSERT(solver_.info() == Success);
  cout << "......done\n";
  return 0;
}

int proj_dyn_tet_solver::advance(double *x) const {
  int rtn = 0;
  switch ( args_.method ) {
    case 0: rtn = advance_alpha(x); break;
    case 1: rtn = advance_beta(x); break;
    case 2: rtn = advance_gamma(x); break;
    default: return __LINE__;
  }
  return rtn;
}

static std::vector<Eigen::Vector3d> lie_pts;

int proj_dyn_tet_solver::advance_alpha(double *x) const { /// @brief Direct
  ASSERT(args_.method == 0);
  Map<VectorXd> X(x, dim_);
  VectorXd xstar = X;
  const auto arap = dynamic_pointer_cast<tet_arap_energy>(ebf_[1]);
  lie_pts.clear();
  // iterate solve
  double prev_jac_norm = -1;
  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
    if ( iter % 10 == 0 ) {
      double value = 0;
      energy_->Val(&xstar[0], &value);
      cout << "\t@iter " << iter << " energy value: " << value << endl;
    }
    arap->LocalSolve(&xstar[0]);
    {
      const size_t tid = 5;
      Matrix3d R(arap->get_aux_var()+9*tid);
      Matrix3d logR = R.log();
      lie_pts.push_back(Vector3d(logR(2, 1), logR(0, 2), logR(1, 0)));
    }
    VectorXd jac = VectorXd::Zero(dim_); {
      energy_->Gra(&xstar[0], &jac[0]);
      jac *= -1;
    }
    double curr_jac_norm = jac.norm();
    if ( curr_jac_norm <= args_.eps ) {
      cout << "\t@CONVERGED after " << iter << " iterations\n";
      break;
    }
    if ( iter % 10 == 0 ) {
      cout << "\t@iter " << iter << " error: " << curr_jac_norm << endl;
      cout << "\t@spectral radius: " << curr_jac_norm/prev_jac_norm << endl << endl;
    }
    prev_jac_norm = curr_jac_norm;
    xstar += solver_.solve(jac);
  }
  // update configuration
  dynamic_pointer_cast<momentum_potential>(ebf_[0])->Update(&xstar[0]);
  X = xstar;
  return 0;
}

int proj_dyn_tet_solver::advance_beta(double *x) const { /// @brief Direct+Chebyshev
  ASSERT(args_.method == 1);
  Map<VectorXd> X(x, dim_);
  VectorXd xstar = X, prev_xstar = xstar, curr_xstar(dim_), dx(dim_);
  const auto arap = dynamic_pointer_cast<tet_arap_energy>(ebf_[1]);
  const size_t S = 10;
  const double rho = 0.87375, gamma = 0.75;
  // iterative solve
  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
    if ( iter % 10 == 0 ) {
      double value = 0;
      energy_->Val(&xstar[0], &value);
      cout << "\t@iter " << iter << " energy value: " << value << endl;
    }
    arap->LocalSolve(&xstar[0]);
    VectorXd jac = VectorXd::Zero(dim_); {
      energy_->Gra(&xstar[0], &jac[0]);
      jac *= -1;
    }
    double curr_jac_norm = jac.norm();
    if ( curr_jac_norm <= args_.eps ) {
      cout << "\t@CONVERGED after " << iter << " iterations\n";
      break;
    }
    if ( iter % 10 == 0 ) {
      cout << "\t@iter " << iter << " error: " << jac.norm() << endl << endl;
    }
    dx = solver_.solve(jac);
    double omega;
    if ( iter < S ) // delay the chebyshev iteration
      omega = 1.0;
    else if ( iter == S )
      omega = 2.0/(2.0-rho*rho);
    else
      omega = 4.0/(4.0-rho*rho*omega);
    curr_xstar = xstar;
    xstar = omega*(gamma*dx+curr_xstar-prev_xstar)+prev_xstar;
    prev_xstar = curr_xstar;
  }
  dynamic_pointer_cast<momentum_potential>(ebf_[0])->Update(&xstar[0]);
  X = xstar;
  return 0;
}

int proj_dyn_tet_solver::advance_gamma(double *x) const { /// @brief Jacobi+Chebyshev
  ASSERT(args_.method == 2);
  Map<VectorXd> X(x, dim_);
  VectorXd xstar = X, prev_xstar = xstar, dx(dim_), curr_xstar(dim_);
  const auto arap = dynamic_pointer_cast<tet_arap_energy>(ebf_[1]);
  const size_t S = 10;
  const double rho = 0.87375, gamma = 0.75;
  // iterative solve
  for (size_t iter = 0; iter < args_.maxiter; ++iter) {
    if ( iter % 10 == 0 ) {
      double value = 0;
      energy_->Val(&xstar[0], &value);
      cout << "\t@iter " << iter << " energy value: " << value << endl;
    }
    arap->LocalSolve(&xstar[0]);
    VectorXd jac = VectorXd::Zero(dim_); {
      energy_->Gra(&xstar[0], &jac[0]);
      jac *= -1;
    }
    double curr_jac_norm = jac.norm();
    if ( curr_jac_norm <= args_.eps ) {
      cout << "\t@CONVERGED after " << iter << " iterations\n";
      break;
    }
    if ( iter % 10 == 0 ) {
      cout << "\t@iter " << iter << " error: " << jac.norm() << endl << endl;
    }
    dx.setZero();
    apply_jacobi(LHS_, jac, dx);
    double omega;
    if ( iter < S ) // delay the chebyshev iteration
      omega = 1.0;
    else if ( iter == S )
      omega = 2.0/(2.0-rho*rho);
    else
      omega = 4.0/(4.0-rho*rho*omega);
    curr_xstar = xstar;
    xstar = omega*(gamma*dx+curr_xstar-prev_xstar)+prev_xstar;
    prev_xstar = curr_xstar;
  }
  dynamic_pointer_cast<momentum_potential>(ebf_[0])->Update(&xstar[0]);
  X = xstar;
  return 0;
}

void proj_dyn_tet_solver::vis_rot(const char *filename) const {
  if ( args_.method != 0 )
    return;
  cout << "[info] visualize the rotation in log space\n";
  vector<double> pts;
  for (size_t i = 0; i < lie_pts.size(); ++i) {
    pts.push_back(lie_pts[i].x());
    pts.push_back(lie_pts[i].y());
    pts.push_back(lie_pts[i].z());
  }
  ofstream os(filename);
  mati_t p = colon(0, lie_pts.size()-1);
  point2vtk(os, &pts[0], pts.size()/3, p.begin(), p.size());
}

}
