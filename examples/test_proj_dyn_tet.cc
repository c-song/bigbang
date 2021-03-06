#include <iostream>
#include <fstream>
#include <jtflib/mesh/io.h>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "src/vtk.h"
#include "src/io.h"
#include "src/proj_dynamics.h"

using namespace std;
using namespace bigbang;
using namespace zjucad::matrix;
namespace po=boost::program_options;

namespace test_proj_dyn_tet {
struct argument {
  string input_mesh;
  string input_cons;
  string input_handle;
  string output_folder;
  size_t total_frame;
  proj_dyn_args proj_args;
};
}

#define APPLY_FORCE(frame, id, f)    \
  if ( i == frame )                  \
    solver.apply_force(id, f);

#define REMOVE_FORCE(frame, id)      \
  if ( i == frame )                  \
    solver.remove_force(id);

#define RELEASE_VERT(frame, id)      \
  if ( i == frame )                  \
    solver.release_vert(id);

int main(int argc, char *argv[])
{
  po::options_description desc("Available options");
  desc.add_options()
      ("help,h", "produce help message")
      ("input_mesh,i", po::value<string>(), "set the input mesh")
      ("input_cons,c", po::value<string>(), "set the input positional constraints")
      ("input_handle,f", po::value<string>(), "set the handles")
      ("output_folder,o", po::value<string>(), "set the output folder")
      ("total_frame,n", po::value<size_t>()->default_value(300), "set the frame number")
      ("method", po::value<int>()->default_value(0), "choose the method")
      ("spectral_radius", po::value<double>()->default_value(0.0), "set the spectral radius")
      ("density,d", po::value<double>()->default_value(1.0), "set the density")
      ("timestep,t", po::value<double>()->default_value(0.01), "set the timestep")
      ("maxiter,m", po::value<size_t>()->default_value(10000), "set the maximum iteration")
      ("tolerance,e", po::value<double>()->default_value(1e-8), "set the tolerance")
      ("ws", po::value<double>()->default_value(1e3), "set the arap weight")
      ("wg", po::value<double>()->default_value(1.0), "set the gravity weight")
      ("wp", po::value<double>()->default_value(1e3), "set the position weight")
      ;
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
  if ( vm.count("help") ) {
    cout << desc << endl;
    return __LINE__;
  }
  test_proj_dyn_tet::argument args; {
    args.input_mesh = vm["input_mesh"].as<string>();
    args.input_cons = vm["input_cons"].as<string>();
    args.input_handle = vm["input_handle"].as<string>();
    args.output_folder = vm["output_folder"].as<string>();
    args.total_frame = vm["total_frame"].as<size_t>();
    args.proj_args.rho = vm["density"].as<double>();
    args.proj_args.h = vm["timestep"].as<double>();
    args.proj_args.maxiter = vm["maxiter"].as<size_t>();
    args.proj_args.method = vm["method"].as<int>();
    args.proj_args.sr = vm["spectral_radius"].as<double>();
    args.proj_args.eps = vm["tolerance"].as<double>();
    args.proj_args.ws = vm["ws"].as<double>();
    args.proj_args.wg = vm["wg"].as<double>();
    args.proj_args.wp = vm["wp"].as<double>();
  }

  if ( !boost::filesystem::exists(args.output_folder) )
    boost::filesystem::create_directory(args.output_folder);

  // load input
  mati_t tets; matd_t nods;
  jtf::mesh::tet_mesh_read_from_zjumat(args.input_mesh.c_str(), &nods, &tets);
  vector<size_t> fixed, driver;
  read_fixed_verts(args.input_cons.c_str(), fixed);
  read_fixed_verts(args.input_handle.c_str(), driver);

  // init the solver
  proj_dyn_tet_solver solver(tets, nods);
  solver.initialize(args.proj_args);

  // initial boudary conditions
  for (auto &elem : fixed)
    solver.pin_down_vert(elem, &nods(0, elem));

  // precompute
  solver.precompute();

  const double intensity = 60;
  char outfile[256];
  for (size_t i = 0; i < args.total_frame; ++i) {
    cout << "[info] frame " << i << endl;
    sprintf(outfile, "%s/frame_method%d_ws%.1e_wg%.1e_wp%.1e_m%zu_%zu.vtk",
            args.output_folder.c_str(), args.proj_args.method, args.proj_args.ws,
            args.proj_args.wg, args.proj_args.wp, args.proj_args.maxiter, i);
    ofstream os(outfile);
    tet2vtk(os, &nods[0], nods.size(2), &tets[0], tets.size(2));

    // apply twist
    matd_t n = cross(nods(colon(), 306)-nods(colon(), 307), nods(colon(), 301)-nods(colon(), 306));
    matd_t o = (nods(colon(), 306)+nods(colon(), 307)+nods(colon(), 301))*ones<double>(3, 1)/3.0;
    if ( i < 100 ) {
      for (auto &pi : driver) {
        matd_t force = cross(n, nods(colon(), pi)-o);
        force = intensity*force/norm(force);
        APPLY_FORCE(i, pi, &force[0]);
      }
    }
    // release twist
    for (auto &pi : driver) {
      REMOVE_FORCE(100, pi);
    }

    solver.advance(&nods[0]);

    sprintf(outfile, "%s/rot_%zu.vtk", args.output_folder.c_str(), i);
    solver.vis_rot(outfile);
  }

  cout << "[info] all done\n";
  return 0;
}
