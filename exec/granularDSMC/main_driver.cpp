#include "INS_functions.H"
#include "species.H"
#include "paramPlane.H"
#include "StructFact.H"
#include "particle_functions.H"
#include "chrono"
#include "iostream"
#include "fstream"
#include "DsmcParticleContainer.H"
#include <AMReX_MultiFab.H>
#include <AMReX_PlotFileUtil.H>

using namespace std::chrono;
using namespace std;
// argv contains the name of the inputs file entered at the command line
void main_driver(const char* argv)
{
	// timer for total simulation time
	Real strt_time = ParallelDescriptor::second();

	std::string inputs_file = argv;

	InitializeCommonNamespace();
	InitializeGmresNamespace();

	BoxArray ba;
	IntVect dom_lo(AMREX_D_DECL(           0,            0,            0));
	IntVect dom_hi(AMREX_D_DECL(n_cells[0]-1, n_cells[1]-1, n_cells[2]-1));
	Box domain(dom_lo, dom_hi);
	DistributionMapping dmap;

	int ncon  = (nspecies+1)*5;
	MultiFab cuInst, cuMeans, cuVars;
	int nprim = (nspecies+1)*9;
	MultiFab primInst, primMeans, primVars;
	int ncovar = 25;
	MultiFab coVars;

	// For long-range temperature-related correlations
	MultiFab cvlMeans, cvlInst, QMeans;

  // see stats for the list
	int ncross = 50;
  MultiFab spatialCross1D;
  
  // For statisitics from particles
 	int npart = 10;
  MultiFab vmom;
  
  // For time correlation functions
  // plot_time is the increments between correlations
  MultiFab rhotimecross, utimecross, Ktimecross;
  MultiFab rho_time, u_time, K_time;

	MultiFab timeCross, t0Cross; // temporary to check if compiles

  if ((plot_cross>0) and ((cross_cell <= 0) or (cross_cell >= n_cells[0]-1)))
  {
      Abort("Cross cell needs to be within the domain: 0 < cross_cell < n_cells[0] - 1");
  }
  if (do_slab_sf)
  {
      Abort("Slab structure factor needs a membrane cell within the domain: 0 < cross_cell < n_cells[0] - 1");
  }

	int step = 0;
	Real dt = fixed_dt;
	bool dt_const=false;
	if(fixed_dt>0) {
		dt_const=true;
	}

	int statsCount = 1; int statsTime = 1;
	Real time = 0.;

	if (restart < 0)
	{
		if (seed > 0)
		{
			InitRandom(seed+ParallelDescriptor::MyProc(),
				ParallelDescriptor::NProcs(),
				seed+ParallelDescriptor::MyProc());
		}
		else if (seed == 0)
		{
			auto now = time_point_cast<nanoseconds>(system_clock::now());
			int randSeed = now.time_since_epoch().count();
			// broadcast the same root seed to all processors
			ParallelDescriptor::Bcast(&randSeed,1,ParallelDescriptor::IOProcessorNumber());
      InitRandom(randSeed+ParallelDescriptor::MyProc(),
									ParallelDescriptor::NProcs(),
									randSeed+ParallelDescriptor::MyProc());
		}
		else
		{
			Abort("Must supply non-negative seed");
		}

		ba.define(domain);
		ba.maxSize(IntVect(max_grid_size));
		dmap.define(ba);
		//////////////////////////////////////
		// Conserved/Primitive Var Setup
		//////////////////////////////////////
		/*
			Conserved Vars:
			0  - rho = (1/V) += m
			1  - Jx  = (1/V) += mu
			2  - Jy  = (1/V) += mv
			3  - Jz  = (1/V) += mw
			4  - K   = (1/V) += m|v|^2
			... (repeat for each species)
		*/

		cuInst.define(ba, dmap, ncon, 0); cuInst.setVal(0.);
		cuMeans.define(ba, dmap, ncon, 0); cuMeans.setVal(0.);
		cuVars.define(ba,dmap, ncon, 0); cuVars.setVal(0.);

		/*
		   Primitive Vars:
			0	- n   (n_ns)
			1 - Yk  (Y_ns)
			2 - u   (u_ns)
			3 - v   (v_ns)
			4 - w   (w_ns)
			5 - G   (G_ns) = dot(u_mean,dJ)
			6 - T   (T_ns)
			7 - P   (P_ns)
			8 - E   (E_ns)
			... (repeat for each species)
		*/

		primInst.define(ba, dmap, nprim, 0); primInst.setVal(0.);
		primMeans.define(ba, dmap, nprim, 0); primMeans.setVal(0.);
		primVars.define(ba, dmap, ncon+nprim, 0); primVars.setVal(0.);

		// Covariances
		/*
			// Conserved
			0  - drho.dJx
			1  - drho.dJy
			2  - drho.dJz
			3  - drho.dK
			4  - dJx.dJy
			5  - dJx.dJz
			6  - dJx.dK
			7  - dJy.dJz
			8  - dJy.dK
			9  - dJz.dk

			// Energy
			10 - drho.dG
			11 - dJx.dG
			12 - dJy.dG
			13 - dJz.dG
			14 - dK.dG

			// Hydro
			15 - drho.du
			16 - drho.dv
			17 - drho.dw
			18 - du.dv
			19 - du.dw
			20 - dv.dw
			21 - drho.dT
			22 - du.dT
			23 - dv.dT
			24 - dw.dT
		*/

		coVars.define(ba, dmap, ncovar, 0);      coVars.setVal(0.);
		// Add flags for spatial correlations
		spatialCross1D.define(ba,dmap,ncross,0); spatialCross1D.setVal(0.);
		// Add flags for time corrleations
		rhotimecross.define(ba,dmap,ntimecor,0); rhotimecross.setVal(0.);
		utimecross.define(ba,dmap,ntimecor,0);   utimecross.setVal(0.);
		Ktimecross.define(ba,dmap,ntimecor,0);   Ktimecross.setVal(0.);

		rho_time.define(ba,dmap,ntimecor,0);     rho_time.setVal(0.);
		u_time.define(ba,dmap,ntimecor,0);       u_time.setVal(0.);
		K_time.define(ba,dmap,ntimecor,0);       K_time.setVal(0.);

		vmom.define(ba,dmap,npart,0);            vmom.setVal(0.);
	}
	else
	{
		ReadCheckPoint(step, time, dt, statsCount, statsTime,
			cuInst, cuMeans, cuVars,
			primInst, primMeans, primVars, coVars,
			spatialCross1D,
			rhotimecross, utimecross, Ktimecross,
			rho_time, u_time, K_time,
			vmom,
			ncon, nprim, ncovar, ncross, ntimecor, npart);
		dmap = cuMeans.DistributionMap();
		ba = cuMeans.boxArray();
	}

	// Specific Heat
	int ncvl = nspecies+1;
	cvlInst.define(ba, dmap, ncvl, 0);  cvlInst.setVal(0.);
	cvlMeans.define(ba, dmap, ncvl, 0); cvlMeans.setVal(0.);
	QMeans.define(ba, dmap, ncvl, 0);   QMeans.setVal(0.);

	Vector<int> is_periodic (AMREX_SPACEDIM,0);
	for (int i=0; i<AMREX_SPACEDIM; ++i)
	{
		if (bc_vel_lo[i] == -1 && bc_vel_hi[i] == -1)
		{
			is_periodic [i] = -1;
		}
	}

	// This defines a Geometry object
	RealBox realDomain({AMREX_D_DECL(prob_lo[0],prob_lo[1],prob_lo[2])},
		{AMREX_D_DECL(prob_hi[0],prob_hi[1],prob_hi[2])});

	Geometry geom (domain ,&realDomain,CoordSys::cartesian,is_periodic.data());

	int paramPlaneCount = 6;
	paramPlane paramPlaneList[paramPlaneCount];
	BuildParamplanes(paramPlaneList,paramPlaneCount,realDomain.lo(),realDomain.hi());

	// Particle tile size
	Vector<int> ts(BL_SPACEDIM);

	for (int d=0; d<AMREX_SPACEDIM; ++d)
	{
		if (max_particle_tile_size[d] > 0)
		{
			ts[d] = max_particle_tile_size[d];
		}
		else
		{
			ts[d] = max_grid_size[d];
		}
	}

	ParmParse pp ("particles");
	pp.addarr("tile_size", ts);

	int cRange = 0;
	FhdParticleContainer particles(geom, dmap, ba, cRange);

	//////////////////////////////////////
	// Structure Factor Setup
	//////////////////////////////////////

	// Output all primitives for structure factor
	int nvarstruct = 6+nspecies*2;
	const Real* dx = geom.CellSize();
	int nstruct = std::ceil((double)nvarstruct*(nvarstruct+1)/2);
	// scale SF results by inverse cell volume
	Vector<Real> var_scaling(nstruct);
	for (int d=0; d<var_scaling.size(); ++d)
	{
		var_scaling[d] = 1./(dx[0]*dx[1]*dx[2]);
	}

	// Structure Factor labels
	Vector< std::string > cu_struct_names(nvarstruct);
	int cnt = 0;
	std::string varname;
	cu_struct_names[cnt++] = "rho";
	for (int ispec=0; ispec<nspecies; ispec++)
	{
		cu_struct_names[cnt++] = amrex::Concatenate("rho",ispec,2);
 	}
	cu_struct_names[cnt++] = "u";
	cu_struct_names[cnt++] = "v";
	cu_struct_names[cnt++] = "w";
	cu_struct_names[cnt++] = "T";
	for (int ispec=0; ispec<nspecies; ispec++)
	{
		cu_struct_names[cnt++] = amrex::Concatenate("T",ispec,2);
	}
	cu_struct_names[cnt++] = "E";

	// Structure Factor
	StructFact structFactPrim  (ba, dmap, cu_struct_names, var_scaling);
	MultiFab   structFactPrimMF(ba, dmap, nvarstruct, 0);

	// Collision Cell Vars
	particles.mfselect.define(ba, dmap, nspecies*nspecies, 0);
	particles.mfselect.setVal(0.);
	particles.mfphi.define(ba, dmap, nspecies, 0);
	particles.mfphi.setVal(0.);
	particles.mfvrmax.define(ba, dmap, nspecies*nspecies, 0);
	particles.mfvrmax.setVal(0.);

	if (restart < 0 && particle_restart < 0)
	{
		particles.InitParticles(dt);
	}
	else
	{
		ReadCheckPointParticles(particles);
	}
	particles.InitCollisionCells();

	Real init_time = ParallelDescriptor::second() - strt_time;
	ParallelDescriptor::ReduceRealMax(init_time);
	amrex::Print() << "Initialization time = " << init_time << " seconds " << std::endl;

	int IO_int = std::ceil(max_step*0.000001);
	IO_int = std::max(IO_int,1);
	max_step += step;
	n_steps_skip += step;
	Real tbegin, tend;
	
	// for geometry
	fixed_dt = dt;
	ParallelDescriptor::Bcast(&fixed_dt,1,ParallelDescriptor::IOProcessorNumber());
	
	particles.vreijrun = 0.0;
	particles.NCollAll = 0.0;
	particles.T0 = -1;
	Print() << "dt: " << dt << "\n";
	Real trun = step*dt;
	for (int istep=step; istep<=max_step; ++istep)
	{
		if(istep%IO_int == 0)
		{
			tbegin = ParallelDescriptor::second();
		}

		//////////////////////////////////////
		// Initial Condition
		//////////////////////////////////////
		if(istep==step)
		{
			particles.EvaluateStats(cuInst,cuMeans,cuVars,primInst,primMeans,primVars,
				cvlInst,cvlMeans,QMeans,coVars,spatialCross1D,statsCount,time);
			if(plot_vmom>0)
			{
				particles.EvaluateStatsPart(vmom);
			}
			if(plot_int>0)
			{
				writePlotFile(cuInst,cuMeans,cuVars,
					primInst,primMeans,primVars,coVars,
					spatialCross1D,rhotimecross,utimecross,Ktimecross,vmom,
					particles,geom,time,
					ncon,nprim,ncovar,ncross,ntimecor,npart,
					istep);
			}
		}

		//////////////////////////////////////
		// DSMC Collide + Move
		//////////////////////////////////////

		particles.CalcSelections(dt);
		particles.CollideParticles(dt);
		particles.Source(dt, paramPlaneList, paramPlaneCount);
		//particles.externalForce(dt);

		// Update 
		trun += dt;
		update_Twall(paramPlaneList, trun);
		particles.MoveParticlesCPP(dt, paramPlaneList, paramPlaneCount);
		//if(!dt_const) {
		//	particles.updateTimeStep(geom,dt);N
		//}

		//////////////////////////////////////
		// Stats
		//////////////////////////////////////

		if (istep >= n_steps_skip)
		{
			particles.EvaluateStats(cuInst,cuMeans,cuVars,primInst,primMeans,primVars,
				cvlInst,cvlMeans,QMeans,coVars,spatialCross1D,statsCount,time);
			vmom.setVal(0.);

			particles.EvaluateStatsPart(vmom);
			if(plot_time>0 && istep%plot_time == 0 && ntimecor>0)
			{
				particles.updateTimeData(cuInst,primInst,
					rho_time,u_time,K_time,ntimecor);
				if(statsTime>ntimecor)
				{
					particles.TimeCorrelation(rho_time, u_time, K_time,
						rhotimecross,utimecross,Ktimecross,
						ntimecor,statsTime-ntimecor);
				}
				statsTime++;
			}
			statsCount++;
		}

		//////////////////////////////////////
		// PlotFile
		//////////////////////////////////////

		bool writePlt = false;
		if (plot_int > 0 && istep>0 && istep>=n_steps_skip)
		{
			if (n_steps_skip >= 0) // for positive n_steps_skip, write out at plot_int
			{
				writePlt = (istep%plot_int == 0);
			}
			else if (n_steps_skip < 0) // for negative n_steps_skip, write out at plot_int-1
			{
				writePlt = ((istep+1)%plot_int == 0);
			}
		}

		if (writePlt)
		{
			writePlotFile(cuInst,cuMeans,cuVars,
				primInst,primMeans,primVars,coVars,
				spatialCross1D,rhotimecross,utimecross,Ktimecross,vmom,
				particles,geom,time,
				ncon,nprim,ncovar,ncross,ntimecor,npart,
				istep);
		}

		//////////////////////////////////////
		// Structure Factor
		//////////////////////////////////////

		if(istep > amrex::Math::abs(n_steps_skip) && struct_fact_int > 0 &&
			(istep-amrex::Math::abs(n_steps_skip))%struct_fact_int == 0)
		{

			int cnt_sf, numvars_sf;
			cnt_sf = 0;
			// rho
			numvars_sf = 1;
			MultiFab::Copy(structFactPrimMF,primInst,0,cnt_sf,numvars_sf,0);
			cnt_sf += numvars_sf;
			// rho species
			for (int i=0;i<nspecies;i++)
			{
				numvars_sf = 1;
				MultiFab::Copy(structFactPrimMF,primInst,1+(i+1)*9,cnt_sf,numvars_sf,0);
				cnt_sf += numvars_sf;
			}
			// u, v, w
			numvars_sf = 3;
			MultiFab::Copy(structFactPrimMF,primInst,2,cnt_sf,numvars_sf,0);
			 cnt_sf += numvars_sf;
			// T
			numvars_sf = 1;
			MultiFab::Copy(structFactPrimMF,primInst,6,cnt_sf,numvars_sf,0);
			cnt_sf += numvars_sf;
			// T species
			for (int i=0;i<nspecies;i++)
			{
				numvars_sf = 1;
				MultiFab::Copy(structFactPrimMF,primInst,6+(i+1)*9,cnt_sf,numvars_sf,0);
				cnt_sf += numvars_sf;
			}
			// E
			numvars_sf = 1;
			MultiFab::Copy(structFactPrimMF,primInst,8,cnt_sf,numvars_sf,0);
			cnt_sf += numvars_sf;

			structFactPrim.FortStructure(structFactPrimMF,geom);
		}

		if(istep > amrex::Math::abs(n_steps_skip) &&
			struct_fact_int > 0 && plot_int > 0 &&
			istep%plot_int == 0)
		{
			structFactPrim.WritePlotFile(istep,time,geom,"plt_SF_prim");
		}

		//////////////////////////////////////
		// Checkpoint
		//////////////////////////////////////

		if (chk_int > 0 && istep%chk_int == 0 && istep > step)
		{
			WriteCheckPoint(istep, time, dt, statsCount, statsTime,
				cuInst, cuMeans, cuVars,
				primInst, primMeans, primVars, coVars,
				particles, spatialCross1D,
				rhotimecross, utimecross, Ktimecross,
				rho_time, u_time, K_time,
				vmom);
		}

		if (istep%IO_int == 0)
		{
			tend = ParallelDescriptor::second() - tbegin;
			ParallelDescriptor::ReduceRealMax(tend);
			amrex::Print() << "Advanced step " << istep << " in " << tend << " seconds\n";
			amrex::Print() << "Time: " << time << " s. dt:  " << dt << " s\n";
		}
		time += dt;
	}

	Real stop_time = ParallelDescriptor::second() - strt_time;
	ParallelDescriptor::ReduceRealMax(stop_time);
	amrex::Print() << "Run time = " << stop_time << " seconds" << std::endl;
}