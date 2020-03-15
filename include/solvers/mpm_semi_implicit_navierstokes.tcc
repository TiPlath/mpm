//! Constructor
template <unsigned Tdim>
mpm::MPMSemiImplicitNavierStokes<Tdim>::MPMSemiImplicitNavierStokes(
    const std::shared_ptr<IO>& io)
    : mpm::MPMBase<Tdim>(io) {
  //! Logger
  console_ = spdlog::get("MPMSemiImplicitNavierStokes");
}

//! MPM semi-implicit two phase solver
template <unsigned Tdim>
bool mpm::MPMSemiImplicitNavierStokes<Tdim>::solve() {
  bool status = true;

  console_->info("MPM analysis type {}", io_->analysis_type());

  // Initialise MPI rank and size
  int mpi_rank = 0;
  int mpi_size = 1;

#ifdef USE_MPI
  // Get MPI rank
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  // Get number of MPI ranks
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
#endif

  // This solver consider only fluid variables
  // NOTE: Due to indexing purposes
  const unsigned fluid = mpm::ParticlePhase::SinglePhase;

  // Test if checkpoint resume is needed
  bool resume = false;
  if (analysis_.find("resume") != analysis_.end())
    resume = analysis_["resume"]["resume"].template get<bool>();

  // Pressure smoothing
  if (analysis_.find("pressure_smoothing") != analysis_.end())
    pressure_smoothing_ = analysis_["pressure_smoothing"].template get<bool>();

  // Projection method parameter (beta)
  if (analysis_.find("semi_implicit") != analysis_.end())
    beta_ = analysis_["semi_implicit"]["beta"].template get<double>();

  // Initialise material
  bool mat_status = this->initialise_materials();
  if (!mat_status) {
    status = false;
    throw std::runtime_error("Initialisation of materials failed");
  }

  // Initialise mesh
  bool mesh_status = this->initialise_mesh();
  if (!mesh_status) {
    status = false;
    throw std::runtime_error("Initialisation of mesh failed");
  }

  // Initialise particles
  bool particle_status = this->initialise_particles();
  if (!particle_status) {
    status = false;
    throw std::runtime_error("Initialisation of particles failed");
  }

  // Initialise loading conditions
  bool loading_status = this->initialise_loads();
  if (!loading_status) {
    status = false;
    throw std::runtime_error("Initialisation of loads failed");
  }

  // Initialise matrix
  bool matrix_status = this->initialise_matrix();
  if (!matrix_status) {
    status = false;
    throw std::runtime_error("Initialisation of matrix failed");
  }

  // Compute mass for each phase
  mesh_->iterate_over_particles(
      std::bind(&mpm::ParticleBase<Tdim>::compute_mass, std::placeholders::_1));

  // Assign beta to each particle
  mesh_->iterate_over_particles(
      std::bind(&mpm::ParticleBase<Tdim>::assign_projection_parameter,
                std::placeholders::_1, beta_));

  // Check point resume
  if (resume) this->checkpoint_resume();

  auto solver_begin = std::chrono::steady_clock::now();
  // Main loop
  for (; step_ < nsteps_; ++step_) {
    if (mpi_rank == 0) console_->info("Step: {} of {}.\n", step_, nsteps_);

    // Create a TBB task group
    tbb::task_group task_group;

    // Spawn a task for initialising nodes and cells
    task_group.run([&] {
      // Initialise nodes
      mesh_->iterate_over_nodes(
          std::bind(&mpm::NodeBase<Tdim>::initialise, std::placeholders::_1));

      mesh_->iterate_over_cells(
          std::bind(&mpm::Cell<Tdim>::activate_nodes, std::placeholders::_1));
    });
    task_group.wait();

    // Spawn a task for particles
    task_group.run([&] {
      // Iterate over each particle to compute shapefn
      mesh_->iterate_over_particles(std::bind(
          &mpm::ParticleBase<Tdim>::compute_shapefn, std::placeholders::_1));
    });

    task_group.wait();

    // Assign mass and momentum to nodes
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::map_mass_momentum_to_nodes,
                  std::placeholders::_1));

    // Compute nodal velocity at the begining of time step
    mesh_->iterate_over_nodes_predicate(
        std::bind(&mpm::NodeBase<Tdim>::compute_velocity,
                  std::placeholders::_1),
        std::bind(&mpm::NodeBase<Tdim>::status, std::placeholders::_1));

    // Iterate over each particle to compute strain rate
    mesh_->iterate_over_particles(std::bind(
        &mpm::ParticleBase<Tdim>::compute_strain, std::placeholders::_1, dt_));

    // Iterate over each particle to compute shear (deviatoric) stress
    mesh_->iterate_over_particles(std::bind(
        &mpm::ParticleBase<Tdim>::compute_stress, std::placeholders::_1));

    // Spawn a task for external force
    task_group.run([&] {
      // Iterate over particles to compute nodal body force
      mesh_->iterate_over_particles(
          std::bind(&mpm::ParticleBase<Tdim>::map_body_force,
                    std::placeholders::_1, this->gravity_));

      // Apply particle traction and map to nodes
      mesh_->apply_traction_on_particles(this->step_ * this->dt_);
    });

    // Spawn a task for internal force
    task_group.run([&] {
      // Iterate over particles to compute nodal internal force
      mesh_->iterate_over_particles(std::bind(
          &mpm::ParticleBase<Tdim>::map_internal_force, std::placeholders::_1));
    });
    task_group.wait();

    // Compute free surface cells, nodes, and particles
    mesh_->compute_free_surface(volume_tolerance_);

    // Compute intermediate velocity
    mesh_->iterate_over_nodes_predicate(
        std::bind(&mpm::NodeBase<Tdim>::compute_acceleration_velocity,
                  std::placeholders::_1, fluid, this->dt_),
        std::bind(&mpm::NodeBase<Tdim>::status, std::placeholders::_1));

    // Reinitialise system matrix to perform PPE
    bool matrix_reinitialization_status = this->reinitialise_matrix();
    if (!matrix_reinitialization_status) {
      status = false;
      throw std::runtime_error("Reinitialisation of matrix failed");
    }

    // Compute poisson equation
    this->compute_poisson_equation();

    // Assign pressure to nodes
    mesh_->iterate_over_nodes_predicate(
        std::bind(&mpm::NodeBase<Tdim>::update_pressure_increment,
                  std::placeholders::_1,
                  matrix_assembler_->pressure_increment(), fluid,
                  this->step_ * this->dt_),
        std::bind(&mpm::NodeBase<Tdim>::status, std::placeholders::_1));

    // Use nodal pressure to update particle pressure
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::compute_updated_pressure,
                  std::placeholders::_1));

    // Compute corrected force
    this->compute_correction_force();

    // Compute corrected acceleration and velocity
    mesh_->iterate_over_nodes_predicate(
        std::bind(
            &mpm::NodeBase<
                Tdim>::compute_acceleration_velocity_navierstokes_semi_implicit,
            std::placeholders::_1, fluid, this->dt_),
        std::bind(&mpm::NodeBase<Tdim>::status, std::placeholders::_1));

    // Update particle position and kinematics
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::compute_updated_position,
                  std::placeholders::_1, this->dt_, velocity_update_));

    // Apply particle velocity constraints
    mesh_->apply_particle_velocity_constraints();

    // Pressure smoothing
    if (pressure_smoothing_) this->pressure_smoothing(fluid);

    // Locate particles
    auto unlocatable_particles = mesh_->locate_particles_mesh();

    if (!unlocatable_particles.empty())
      throw std::runtime_error("Particle outside the mesh domain");

    if (step_ % output_steps_ == 0) {
      // HDF5 outputs
      this->write_hdf5(this->step_, this->nsteps_);
#ifdef USE_VTK
      // VTK outputs
      this->write_vtk(this->step_, this->nsteps_);
#endif
    }
  }
  auto solver_end = std::chrono::steady_clock::now();
  console_->info("Rank {}, SemiImplicit_NavierStokes {} solver duration: {} ms",
                 mpi_rank,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     solver_end - solver_begin)
                     .count());

  return status;
}

// Semi-implicit functions
// Initialise matrix
template <unsigned Tdim>
bool mpm::MPMSemiImplicitNavierStokes<Tdim>::initialise_matrix() {
  bool status = true;
  try {
    // Max iteration steps
    unsigned max_iter =
        analysis_["matrix"]["max_iter"].template get<unsigned>();
    // Tolerance
    double tolerance = analysis_["matrix"]["tolerance"].template get<double>();
    // Get matrix assembler type
    std::string assembler_type =
        analysis_["matrix"]["assembler_type"].template get<std::string>();
    // Get matrix solver type
    std::string solver_type =
        analysis_["matrix"]["solver_type"].template get<std::string>();
    // Get volume tolerance for free surface
    volume_tolerance_ =
        analysis_["matrix"]["volume_tolerance"].template get<double>();
    // Create matrix assembler
    matrix_assembler_ =
        Factory<mpm::AssemblerBase<Tdim>>::instance()->create(assembler_type);
    // Create matrix solver
    matrix_solver_ =
        Factory<mpm::SolverBase<Tdim>, unsigned, double>::instance()->create(
            solver_type, std::move(max_iter), std::move(tolerance));
    // Assign mesh pointer to assembler
    matrix_assembler_->assign_mesh_pointer(mesh_);

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

// Reinitialise and resize matrices at the beginning of every time step
template <unsigned Tdim>
bool mpm::MPMSemiImplicitNavierStokes<Tdim>::reinitialise_matrix() {
  bool status = true;
  try {
    // Assigning matrix id
    const auto nactive_node = mesh_->assign_active_node_id();

    // Assign global node indice
    matrix_assembler_->assign_global_node_indices(nactive_node);

    // Assign pressure constraints
    matrix_assembler_->assign_pressure_constraints(this->beta_,
                                                   this->step_ * this->dt_);

    // Initialise element matrix
    mesh_->iterate_over_cells(std::bind(
        &mpm::Cell<Tdim>::initialise_element_matrix, std::placeholders::_1));

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

// FIXME: This is a copy of pressure_smoothing in explicit two-phase
//! MPM Explicit two-phase pressure smoothing
template <unsigned Tdim>
void mpm::MPMSemiImplicitNavierStokes<Tdim>::pressure_smoothing(
    unsigned phase) {

  // Map pressures to nodes
  mesh_->iterate_over_particles(std::bind(
      &mpm::ParticleBase<Tdim>::map_pressure_to_nodes, std::placeholders::_1));

#ifdef USE_MPI
  int mpi_size = 1;

  // Get number of MPI ranks
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);

  // Run if there is more than a single MPI task
  if (mpi_size > 1) {
    // MPI all reduce nodal pressure
    mesh_->template nodal_halo_exchange<double, 1>(
        std::bind(&mpm::NodeBase<Tdim>::pressure, std::placeholders::_1, phase),
        std::bind(&mpm::NodeBase<Tdim>::assign_pressure, std::placeholders::_1,
                  phase, std::placeholders::_2));
  }
#endif

  // Map Pressure back to particles
  mesh_->iterate_over_particles(
      std::bind(&mpm::ParticleBase<Tdim>::compute_pressure_smoothing,
                std::placeholders::_1));
}

// Compute poisson equation
template <unsigned Tdim>
bool mpm::MPMSemiImplicitNavierStokes<Tdim>::compute_poisson_equation(
    std::string solver_type) {
  bool status = true;
  try {
    // Construct local cell laplacian matrix
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::map_laplacian_to_cell,
                  std::placeholders::_1));

    // Assemble global laplacian matrix
    matrix_assembler_->assemble_laplacian_matrix(dt_);

    // Map Poisson RHS matrix
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::map_poisson_right_to_cell,
                  std::placeholders::_1));

    // Assemble poisson RHS vector
    matrix_assembler_->assemble_poisson_right(mesh_, dt_);

    // Assign free surface to assembler
    matrix_assembler_->assign_free_surface(mesh_->free_surface_nodes());

    // Apply constraints
    matrix_assembler_->apply_pressure_constraints();

    // Solve matrix equation
    matrix_assembler_->assign_pressure_increment(matrix_solver_->solve(
        matrix_assembler_->laplacian_matrix(),
        matrix_assembler_->poisson_rhs_vector(), solver_type));

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Compute corrected force
template <unsigned Tdim>
bool mpm::MPMSemiImplicitNavierStokes<Tdim>::compute_correction_force() {
  bool status = true;
  try {
    mesh_->iterate_over_particles(
        std::bind(&mpm::ParticleBase<Tdim>::map_correction_matrix_to_cell,
                  std::placeholders::_1));

    // Assemble correction matrix
    matrix_assembler_->assemble_corrector_right(mesh_, dt_);

    // Assign corrected force
    mesh_->compute_nodal_correction_force(
        matrix_assembler_->correction_matrix(),
        matrix_assembler_->pressure_increment(), dt_);

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}
