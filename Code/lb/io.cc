/*! \file config.cc
 \brief In this file, the functions useful for the input/output are reported
 */
#include <limits.h>
#include <sstream>
#include <math.h>
#include <string.h>

#include "debug/Debugger.h"
#include "lb/lb.h"
#include "net.h"
#include "util/utilityFunctions.h"
#include "io/XdrMemReader.h"
#include "io/XdrMemWriter.h"
#include "io/AsciiFileWriter.h"
#include "topology/TopologyReader.h"

namespace hemelb
{
  namespace lb
  {
    /*!
     this function reads the XDR configuration file but does not store the system
     and calculate some parameters
     */
    void LBM::ReadConfig(hemelb::lb::GlobalLatticeData &bGlobalLatticeData)
    {
      /* Read the config file written by the segtool.
       *
       * All values encoded using XDR format. Uses int, double and u_int.
       *
       * System parameters:
       *   double stress_type
       *   int blocks_x
       *   int blocks_y
       *   int blocks_z
       *   int block_size
       *
       * For each block (all blocks_x * blocks_y * blocks_z of them):
       *
       *   int flag (indicates presence of non-solid sites in the block)
       *
       *   If flag == 0 go to next block
       *
       *   Otherwise for each site in the block (all block_size^3):
       *
       *     u_int site_data -- this is a bit field which indicates site
       *     type (OR with SITE_TYPE_MASK to get bits zero and one; 00 =
       *     solid, 01 = fluid, 10 = inlet, 11 = outlet) or edgeness (set
       *     bit with PRESSURE_EDGE_MASK)
       *
       *     If solid or simple fluid, go to next site
       *
       *     If inlet or outlet (irrespective of edge state) {
       *       double boundary_normal[3]
       *       double boundary_dist
       *     }
       *
       *     If edge bit set {
       *       double wall_normal[3]
       *       double wall_dist
       *     }
       *
       *     double mDistanceToWall[14]
       */

      MPI_File lFile;
      int lError;

      // Open the file using the MPI parallel I/O interface at the path
      // given, in read-only mode.
      lError = MPI_File_open(MPI_COMM_WORLD, &mSimConfig->DataFilePath[0], MPI_MODE_RDONLY,
                             MPI_INFO_NULL, &lFile);

      if (lError != 0)
      {
        fprintf(stderr, "Unable to open file %s [rank %i], exiting\n",
                mSimConfig->DataFilePath.c_str(), mNetTopology->GetLocalRank());
        fflush(0x0);
        exit(0x0);
      }
      else
      {
        fprintf(stderr, "Opened config file %s [rank %i]\n", mSimConfig->DataFilePath.c_str(),
                mNetTopology->GetLocalRank());
      }
      fflush(NULL);

      // Read the preamble.
      hemelb::topology::TopologyReader lTopologyReader;
      lTopologyReader.PreReadConfigFile(lFile, &mParams, bGlobalLatticeData);

      total_fluid_sites = 0;

      site_min_x = INT_MAX;
      site_min_y = INT_MAX;
      site_min_z = INT_MAX;
      site_max_x = INT_MIN;
      site_max_y = INT_MIN;
      site_max_z = INT_MIN;

      // Each block has an int flag, each site has at most an unsigned int, 8 doubles, and (Num-vectors - 1) doubles.
      int lLength = bGlobalLatticeData.GetBlockCount() * (4
          + bGlobalLatticeData.SitesPerBlockVolumeUnit * (4 + 8 * 8 + 8 * (D3Q15::NUMVECTORS - 1)));

      char * lBlockDataBuffer = new char[lLength];

      MPI_Status lStatus;

      MPI_File_read_all(lFile, lBlockDataBuffer, lLength, MPI_BYTE, &lStatus);

      hemelb::io::XdrMemReader myReader = hemelb::io::XdrMemReader(lBlockDataBuffer, lLength);

      for (BlockCounter lBlockCounter(&bGlobalLatticeData, 0); lBlockCounter
          < bGlobalLatticeData.GetBlockCount(); lBlockCounter++)
      {
        bGlobalLatticeData.Blocks[lBlockCounter].site_data = NULL;
        bGlobalLatticeData.Blocks[lBlockCounter].ProcessorRankForEachBlockSite = NULL;
        bGlobalLatticeData.Blocks[lBlockCounter].wall_data = NULL;

        int flag;

        myReader.readInt(flag);

        if (flag == 0)
          continue;
        // Block contains some non-solid sites

        bGlobalLatticeData.Blocks[lBlockCounter].site_data
            = new unsigned int[bGlobalLatticeData.SitesPerBlockVolumeUnit];
        bGlobalLatticeData.Blocks[lBlockCounter].ProcessorRankForEachBlockSite
            = new int[bGlobalLatticeData.SitesPerBlockVolumeUnit];

        int m = -1;

        for (int ii = 0; ii < bGlobalLatticeData.GetBlockSize(); ii++)
        {
          unsigned int site_i = lBlockCounter.GetICoord(ii);

          for (int jj = 0; jj < bGlobalLatticeData.GetBlockSize(); jj++)
          {
            unsigned int site_j = lBlockCounter.GetJCoord(jj);

            for (int kk = 0; kk < bGlobalLatticeData.GetBlockSize(); kk++)
            {
              unsigned int site_k = lBlockCounter.GetKCoord(kk);

              ++m;

              unsigned int *site_type = &bGlobalLatticeData.Blocks[lBlockCounter].site_data[m];
              myReader.readUnsignedInt(*site_type);

              if ( (*site_type & SITE_TYPE_MASK) == hemelb::lb::SOLID_TYPE)
              {
                bGlobalLatticeData.Blocks[lBlockCounter].ProcessorRankForEachBlockSite[m] = 1 << 30;
                continue;
              }
              bGlobalLatticeData.Blocks[lBlockCounter].ProcessorRankForEachBlockSite[m] = -1;

              ++total_fluid_sites;

              site_min_x = hemelb::util::min(site_min_x, site_i);
              site_min_y = hemelb::util::min(site_min_y, site_j);
              site_min_z = hemelb::util::min(site_min_z, site_k);
              site_max_x = hemelb::util::max(site_max_x, site_i);
              site_max_y = hemelb::util::max(site_max_y, site_j);
              site_max_z = hemelb::util::max(site_max_z, site_k);

              if (bGlobalLatticeData.GetCollisionType(*site_type) != FLUID)
              {
                // Neither solid nor simple fluid
                if (bGlobalLatticeData.Blocks[lBlockCounter].wall_data == NULL)
                {
                  bGlobalLatticeData.Blocks[lBlockCounter].wall_data
                      = new hemelb::lb::WallData[bGlobalLatticeData.SitesPerBlockVolumeUnit];
                }

                if (bGlobalLatticeData.GetCollisionType(*site_type) & INLET
                    || bGlobalLatticeData.GetCollisionType(*site_type) & OUTLET)
                {
                  double temp;
                  // INLET or OUTLET or both.
                  // These values are the boundary normal and the boundary distance.
                  for (int l = 0; l < 3; l++)
                    myReader.readDouble(temp);

                  myReader.readDouble(temp);
                }

                if (bGlobalLatticeData.GetCollisionType(*site_type) & EDGE)
                {
                  // EDGE bit set
                  for (int l = 0; l < 3; l++)
                    myReader.readDouble(
                                        bGlobalLatticeData.Blocks[lBlockCounter].wall_data[m].wall_nor[l]);

                  double temp;
                  myReader.readDouble(temp);
                }

                for (unsigned int l = 0; l < (D3Q15::NUMVECTORS - 1); l++)
                  myReader.readDouble(
                                      bGlobalLatticeData.Blocks[lBlockCounter].wall_data[m].cut_dist[l]);
              }
            } // kk
          } // jj
        } // ii
      } // i

      delete[] lBlockDataBuffer;

      MPI_File_close(&lFile);
    }

    // TODO
    /*
     void LBM::ReadBlock(unsigned int * &siteData,
     int * &procRankForEachBlockSite,
     hemelb::lb::WallData * &wallData,
     hemelb::io::XdrReader &reader,
     const hemelb::lb::GlobalLatticeData &iGlobLatDat)
     {
     int flag;

     reader.readInt(flag);

     if (flag == 0)
     continue;
     // Block contains some non-solid sites

     siteData = new unsigned int[iGlobLatDat.SitesPerBlockVolumeUnit];
     procRankForEachBlockSite = new int[iGlobLatDat.SitesPerBlockVolumeUnit];

     int m = -1;

     for (int ii = 0; ii < iGlobLatDat.GetBlockSize(); ii++)
     {
     unsigned int site_i = (i << iGlobLatDat.Log2BlockSize) + ii;

     for (int jj = 0; jj < iGlobLatDat.GetBlockSize(); jj++)
     {
     unsigned int site_j = (j << iGlobLatDat.Log2BlockSize) + jj;

     for (int kk = 0; kk < iGlobLatDat.GetBlockSize(); kk++)
     {
     unsigned int site_k = (k << iGlobLatDat.Log2BlockSize) + kk;

     ++m;

     unsigned int *site_type = &siteData[m];
     reader.readUnsignedInt(*site_type);

     if ( (*site_type & SITE_TYPE_MASK) == hemelb::lb::SOLID_TYPE)
     {
     procRankForEachBlockSite[m] = 1 << 30;
     continue;
     }
     procRankForEachBlockSite[m] = -1;

     ++total_fluid_sites;

     site_min_x = hemelb::util::min(site_min_x, site_i);
     site_min_y = hemelb::util::min(site_min_y, site_j);
     site_min_z = hemelb::util::min(site_min_z, site_k);
     site_max_x = hemelb::util::max(site_max_x, site_i);
     site_max_y = hemelb::util::max(site_max_y, site_j);
     site_max_z = hemelb::util::max(site_max_z, site_k);

     if (net->GetCollisionType(*site_type) != FLUID)
     {
     // Neither solid nor simple fluid
     if (bGlobalLatticeData.Blocks[n].wall_data == NULL)
     {
     bGlobalLatticeData.Blocks[n].wall_data
     = new hemelb::lb::WallData[bGlobalLatticeData.SitesPerBlockVolumeUnit];
     }

     if (net->GetCollisionType(*site_type) & INLET
     || net->GetCollisionType(*site_type) & OUTLET)
     {
     double temp;
     // INLET or OUTLET or both.
     // These values are the boundary normal and the boundary distance.
     for (int l = 0; l < 3; l++)
     myReader.readDouble(temp);

     myReader.readDouble(temp);
     }

     if (net->GetCollisionType(*site_type) & EDGE)
     {
     // EDGE bit set
     for (int l = 0; l < 3; l++)
     myReader.readDouble(
     bGlobalLatticeData.Blocks[n].wall_data[m].wall_nor[l]);

     double temp;
     myReader.readDouble(temp);
     }

     for (unsigned int l = 0; l < (D3Q15::NUMVECTORS - 1); l++)
     myReader.readDouble(
     bGlobalLatticeData.Blocks[n].wall_data[m].cut_dist[l]);
     }
     } // kk
     } // jj
     } // ii
     }
     */

    /*!
     through this function the processor 0 reads the LB parameters
     and then communicate them to the other processors
     */
    void LBM::ReadParameters()
    {

      double par_to_send[10000];
      int nParamsRead = 0;
      int err;

      if (mNetTopology->IsCurrentProcTheIOProc())
      {
        inlets = mSimConfig->Inlets.size();
        allocateInlets(inlets);

        for (int n = 0; n < inlets; n++)
        {
          hemelb::SimConfig::InOutLet *lInlet = mSimConfig->Inlets[n];

          inlet_density_avg[n] = ConvertPressureToLatticeUnits(lInlet->PMean) / Cs2;
          inlet_density_amp[n] = ConvertPressureGradToLatticeUnits(lInlet->PAmp) / Cs2;
          inlet_density_phs[n] = lInlet->PPhase * DEG_TO_RAD;
        }

        outlets = mSimConfig->Outlets.size();
        allocateOutlets(outlets);

        for (int n = 0; n < outlets; n++)
        {
          hemelb::SimConfig::InOutLet *lOutlet = mSimConfig->Outlets[n];
          outlet_density_avg[n] = ConvertPressureToLatticeUnits(lOutlet->PMean) / Cs2;
          outlet_density_amp[n] = ConvertPressureGradToLatticeUnits(lOutlet->PAmp) / Cs2;
          outlet_density_phs[n] = lOutlet->PPhase * DEG_TO_RAD;
        }

        average_inlet_velocity = new double[inlets];
        peak_inlet_velocity = new double[inlets];
        inlet_normal = new double[3 * inlets];
        inlet_count = new long int[inlets];

        is_inlet_normal_available = 1;

        for (int ii = 0; ii < inlets; ii++)
        {
          inlet_normal[3 * ii] = mSimConfig->Inlets[ii]->Normal.x;
          inlet_normal[3 * ii + 1] = mSimConfig->Inlets[ii]->Normal.y;
          inlet_normal[3 * ii + 2] = mSimConfig->Inlets[ii]->Normal.z;
        }

        par_to_send[0] = 0.1 + (double) inlets;
        par_to_send[1] = 0.1 + (double) outlets;
        par_to_send[2] = 0.1 + (double) is_inlet_normal_available;
      }

      err = MPI_Bcast(par_to_send, 3, MPI_DOUBLE, 0, MPI_COMM_WORLD);

      if (!mNetTopology->IsCurrentProcTheIOProc())
      {
        inlets = (int) par_to_send[0];
        outlets = (int) par_to_send[1];
        is_inlet_normal_available = (int) par_to_send[2];

        allocateInlets(inlets);
        allocateOutlets(outlets);

        average_inlet_velocity = new double[inlets];
        peak_inlet_velocity = new double[inlets];
        inlet_normal = new double[3 * inlets];
        inlet_count = new long int[inlets];
      }
      else
      {
        for (int n = 0; n < inlets; n++)
        {
          par_to_send[3 * n + 0] = inlet_density_avg[n];
          par_to_send[3 * n + 1] = inlet_density_amp[n];
          par_to_send[3 * n + 2] = inlet_density_phs[n];
        }
        for (int n = 0; n < outlets; n++)
        {
          par_to_send[3 * inlets + 3 * n + 0] = outlet_density_avg[n];
          par_to_send[3 * inlets + 3 * n + 1] = outlet_density_amp[n];
          par_to_send[3 * inlets + 3 * n + 2] = outlet_density_phs[n];
        }
        if (is_inlet_normal_available)
        {
          for (int n = 0; n < inlets; n++)
          {
            par_to_send[3 * (inlets + outlets) + 3 * n + 0] = inlet_normal[3 * n + 0];
            par_to_send[3 * (inlets + outlets) + 3 * n + 1] = inlet_normal[3 * n + 1];
            par_to_send[3 * (inlets + outlets) + 3 * n + 2] = inlet_normal[3 * n + 2];
          }
        }
      }

      err = MPI_Bcast(par_to_send, 3 * (inlets + outlets + inlets), MPI_DOUBLE, 0, MPI_COMM_WORLD);

      if (!mNetTopology->IsCurrentProcTheIOProc())
      {
        for (int n = 0; n < inlets; n++)
        {
          inlet_density_avg[n] = par_to_send[3 * n + 0];
          inlet_density_amp[n] = par_to_send[3 * n + 1];
          inlet_density_phs[n] = par_to_send[3 * n + 2];
        }
        for (int n = 0; n < outlets; n++)
        {
          outlet_density_avg[n] = par_to_send[3 * inlets + 3 * n + 0];
          outlet_density_amp[n] = par_to_send[3 * inlets + 3 * n + 1];
          outlet_density_phs[n] = par_to_send[3 * inlets + 3 * n + 2];
        }
        if (is_inlet_normal_available)
        {
          for (int n = 0; n < inlets; n++)
          {
            inlet_normal[3 * n + 0] = par_to_send[3 * (inlets + outlets) + 3 * n + 0];
            inlet_normal[3 * n + 1] = par_to_send[3 * (inlets + outlets) + 3 * n + 1];
            inlet_normal[3 * n + 2] = par_to_send[3 * (inlets + outlets) + 3 * n + 2];
          }
        }
      }
      UpdateBoundaryDensities(0, 0);

      RecalculateTauViscosityOmega();
    }

    void LBM::allocateInlets(int nInlets)
    {
      nInlets = hemelb::util::max(1, nInlets);
      inlet_density = new double[nInlets];
      inlet_density_avg = new double[nInlets];
      inlet_density_amp = new double[nInlets];
      inlet_density_phs = new double[nInlets];
    }

    void LBM::allocateOutlets(int nOutlets)
    {
      nOutlets = hemelb::util::max(1, nOutlets);
      outlet_density = new double[nOutlets];
      outlet_density_avg = new double[nOutlets];
      outlet_density_amp = new double[nOutlets];
      outlet_density_phs = new double[nOutlets];
    }

    void LBM::WriteConfig(hemelb::lb::Stability stability,
                          std::string output_file_name,
                          const hemelb::lb::GlobalLatticeData &iGlobalLatticeData,
                          const hemelb::lb::LocalLatticeData &iLocalLatticeData)
    {
      /* This routine writes the flow field on file. The data are gathered
       to the root processor and written from there.  The format
       comprises:

       0- Flag for simulation stability, 0 or 1

       1- Voxel size in physical units (units of m)

       2- vertex coords of the minimum bounding box with minimum values
       (x, y and z values)

       3- vertex coords of the minimum bounding box with maximum values
       (x, y and z values)

       4- #voxels within the minimum bounding box along the x, y, z axes
       (3 values)

       5- total number of fluid voxels

       6-And then a list of the fluid voxels... For each fluid voxel:

       a- the (x, y, z) coordinates in lattice units (3 values)
       b- the pressure in physical units (mmHg)
       c- (x,y,z) components of the velocity field in physical units (3
       values, m/s)
       d- the von Mises stress in physical units (Pa) (the stored shear
       stress is equal to -1 if the fluid voxel is not at the wall)

       */
      hemelb::io::AsciiFileWriter *realSnap = NULL;

      float *local_flow_field, *gathered_flow_field;

      int err;

      double density;
      double pressure;
      double vx, vy, vz;
      double stress;
      double f_eq[D3Q15::NUMVECTORS], f_neq[D3Q15::NUMVECTORS];

      int buffer_size;
      int fluid_sites_max;
      int communication_period, communication_iters;
      int comPeriodDelta, iters;
      int par;
      int shrinked_sites_x, shrinked_sites_y, shrinked_sites_z;

      short int *local_site_data, *gathered_site_data;

      unsigned int my_site_id;

      if (mNetTopology->IsCurrentProcTheIOProc())
      {
        realSnap = new hemelb::io::AsciiFileWriter(output_file_name);
        //snap << stability << snap->eol;
        (*realSnap << stability) << realSnap->eol;
        //snap->write(stability); snap->writeRecordSeparator();
      }
      hemelb::io::Writer& snap = *realSnap;

      if (stability == hemelb::lb::Unstable)
      {
        if (mNetTopology->IsCurrentProcTheIOProc())
        {
          delete realSnap;
        }
        return;
      }

      if (mNetTopology->IsCurrentProcTheIOProc())
      {
        shrinked_sites_x = 1 + site_max_x - site_min_x;
        shrinked_sites_y = 1 + site_max_y - site_min_y;
        shrinked_sites_z = 1 + site_max_z - site_min_z;

        snap << voxel_size << hemelb::io::Writer::eol;
        snap << site_min_x << site_min_y << site_min_z << hemelb::io::Writer::eol;
        snap << site_max_x << site_max_y << site_max_z << hemelb::io::Writer::eol;
        snap << shrinked_sites_x << shrinked_sites_y << shrinked_sites_z << hemelb::io::Writer::eol;
        snap << total_fluid_sites << hemelb::io::Writer::eol;
      }

      fluid_sites_max = 0;

      for (int n = 0; n < mNetTopology->GetProcessorCount(); n++)
      {
        fluid_sites_max = hemelb::util::max(fluid_sites_max,
                                            mNetTopology->FluidSitesOnEachProcessor[n]);
      }

      // "buffer_size" is the size of the flow field buffer to send to the
      // root processor ("local_flow_field") and that to accommodate the
      // received ones from the non-root processors
      // ("gathered_flow_field").  If "buffer_size" is larger the
      // frequency with which data communication to the root processor is
      // performed becomes lower and viceversa
      buffer_size = hemelb::util::min(1000000, fluid_sites_max * mNetTopology->GetProcessorCount());

      communication_period = int (ceil(double (buffer_size) / mNetTopology->GetProcessorCount()));

      communication_iters = hemelb::util::max(1, int (ceil(double (fluid_sites_max)
          / communication_period)));

      local_flow_field = new float[MACROSCOPIC_PARS * communication_period];
      gathered_flow_field = new float[MACROSCOPIC_PARS * communication_period
          * mNetTopology->GetProcessorCount()];

      local_site_data = new short int[3 * communication_period];
      gathered_site_data = new short int[3 * communication_period
          * mNetTopology->GetProcessorCount()];

      for (comPeriodDelta = 0; comPeriodDelta < communication_period; comPeriodDelta++)
      {
        local_site_data[comPeriodDelta * 3] = -1;
      }
      iters = 0;
      comPeriodDelta = 0;

      par = 0;

      /* The following loops scan over every single macrocell (block). If
       the block is non-empty, it scans the fluid sites within that block
       If the site is fluid, it calculates the flow field and then is
       converted to physical units and stored in a buffer to send to the
       root processor */

      int n = -1; // net->proc_block counter
      for (int i = 0; i < iGlobalLatticeData.GetXSiteCount(); i
          += iGlobalLatticeData.GetBlockSize())
      {
        for (int j = 0; j < iGlobalLatticeData.GetYSiteCount(); j
            += iGlobalLatticeData.GetBlockSize())
        {
          for (int k = 0; k < iGlobalLatticeData.GetZSiteCount(); k
              += iGlobalLatticeData.GetBlockSize())
          {

            ++n;

            if (iGlobalLatticeData.Blocks[n].ProcessorRankForEachBlockSite == NULL)
            {
              continue;
            }
            int m = -1;

            for (int site_i = i; site_i < i + iGlobalLatticeData.GetBlockSize(); site_i++)
            {
              for (int site_j = j; site_j < j + iGlobalLatticeData.GetBlockSize(); site_j++)
              {
                for (int site_k = k; site_k < k + iGlobalLatticeData.GetBlockSize(); site_k++)
                {

                  m++;
                  if (mNetTopology->GetLocalRank()
                      != iGlobalLatticeData.Blocks[n].ProcessorRankForEachBlockSite[m])
                  {
                    continue;
                  }

                  my_site_id = iGlobalLatticeData.Blocks[n].site_data[m];

                  /* No idea what this does */
                  if (my_site_id & (1U << 31U))
                    continue;

                  // TODO Utter filth. The cases where the whole site data is exactly equal
                  // to "FLUID_TYPE" and where just the type-component of the whole site data
                  // is equal to "FLUID_TYPE" are handled differently.
                  if (iLocalLatticeData.mSiteData[my_site_id] == hemelb::lb::FLUID_TYPE)
                  {
                    D3Q15::CalculateDensityVelocityFEq(&iLocalLatticeData.FOld[ (my_site_id * (par
                        + 1) + par) * D3Q15::NUMVECTORS], density, vx, vy, vz, f_eq);

                    for (unsigned int l = 0; l < D3Q15::NUMVECTORS; l++)
                    {
                      f_neq[l] = iLocalLatticeData.FOld[ (my_site_id * (par + 1) + par)
                          * D3Q15::NUMVECTORS + l] - f_eq[l];
                    }

                  }
                  else
                  { // not FLUID_TYPE
                    CalculateBC(&iLocalLatticeData.FOld[ (my_site_id * (par + 1) + par)
                        * D3Q15::NUMVECTORS], iLocalLatticeData.GetSiteType(my_site_id),
                                iLocalLatticeData.GetBoundaryId(my_site_id), &density, &vx, &vy,
                                &vz, f_neq);
                  }

                  if (mParams.StressType == hemelb::lb::ShearStress)
                  {
                    if (iLocalLatticeData.GetNormalToWall(my_site_id)[0] >= BIG_NUMBER)
                    {
                      stress = -1.0;
                    }
                    else
                    {
                      D3Q15::CalculateShearStress(
                                                  density,
                                                  f_neq,
                                                  &iLocalLatticeData.GetNormalToWall(my_site_id)[0],
                                                  stress, mParams.StressParameter);
                    }
                  }
                  else
                  {
                    D3Q15::CalculateVonMisesStress(f_neq, stress, mParams.StressParameter);
                  }

                  vx /= density;
                  vy /= density;
                  vz /= density;

                  // conversion from lattice to physical units
                  pressure = ConvertPressureToPhysicalUnits(density * Cs2);

                  vx = ConvertVelocityToPhysicalUnits(vx);
                  vy = ConvertVelocityToPhysicalUnits(vy);
                  vz = ConvertVelocityToPhysicalUnits(vz);

                  stress = ConvertStressToPhysicalUnits(stress);

                  local_flow_field[MACROSCOPIC_PARS * comPeriodDelta + 0] = float (pressure);
                  local_flow_field[MACROSCOPIC_PARS * comPeriodDelta + 1] = float (vx);
                  local_flow_field[MACROSCOPIC_PARS * comPeriodDelta + 2] = float (vy);
                  local_flow_field[MACROSCOPIC_PARS * comPeriodDelta + 3] = float (vz);
                  local_flow_field[MACROSCOPIC_PARS * comPeriodDelta + 4] = float (stress);

                  local_site_data[3 * comPeriodDelta + 0] = site_i;
                  local_site_data[3 * comPeriodDelta + 1] = site_j;
                  local_site_data[3 * comPeriodDelta + 2] = site_k;

                  if (++comPeriodDelta != communication_period)
                    continue;

                  comPeriodDelta = 0;
                  ++iters;

                  err = MPI_Gather(local_flow_field, MACROSCOPIC_PARS * communication_period,
                                   MPI_FLOAT, gathered_flow_field, MACROSCOPIC_PARS
                                       * communication_period, MPI_FLOAT, 0, MPI_COMM_WORLD);

                  err = MPI_Gather(local_site_data, 3 * communication_period, MPI_SHORT,
                                   gathered_site_data, 3 * communication_period, MPI_SHORT, 0,
                                   MPI_COMM_WORLD);

                  if (mNetTopology->IsCurrentProcTheIOProc())
                  {

                    for (int l = 0; l < mNetTopology->GetProcessorCount() * communication_period; l++)
                    {
                      if (gathered_site_data[l * 3 + 0] == -1)
                        continue;

                      gathered_site_data[l * 3 + 0] -= site_min_x;
                      gathered_site_data[l * 3 + 1] -= site_min_y;
                      gathered_site_data[l * 3 + 2] -= site_min_z;

                      snap << gathered_site_data[l * 3 + 0] << gathered_site_data[l * 3 + 1]
                          << gathered_site_data[l * 3 + 2];

                      for (int kk = 0; kk < MACROSCOPIC_PARS; kk++)
                      {
                        snap << gathered_flow_field[MACROSCOPIC_PARS * l + kk];
                      }
                      snap << hemelb::io::Writer::eol;
                    }

                  }

                  for (int l = 0; l < communication_period; l++)
                  {
                    local_site_data[l * 3] = -1;
                  }

                } // for site_k
              } // for site_j
            } // for site_i

          } // for k
        } // for j
      } //for i

      if (iters != communication_iters)
      {
        ++iters;

        // Weirdly initialized for
        for (; iters <= communication_iters; iters++)
        {
          err = MPI_Gather(local_flow_field, MACROSCOPIC_PARS * communication_period, MPI_FLOAT,
                           gathered_flow_field, MACROSCOPIC_PARS * communication_period, MPI_FLOAT,
                           0, MPI_COMM_WORLD);

          err = MPI_Gather(local_site_data, 3 * communication_period, MPI_SHORT,
                           gathered_site_data, 3 * communication_period, MPI_SHORT, 0,
                           MPI_COMM_WORLD);

          if (mNetTopology->IsCurrentProcTheIOProc())
          {
            for (int l = 0; l < mNetTopology->GetProcessorCount() * communication_period; l++)
            {

              if (gathered_site_data[l * 3 + 0] == -1)
                continue;

              gathered_site_data[l * 3 + 0] -= site_min_x;
              gathered_site_data[l * 3 + 1] -= site_min_y;
              gathered_site_data[l * 3 + 2] -= site_min_z;

              snap << gathered_site_data[l * 3 + 0] << gathered_site_data[l * 3 + 1]
                  << gathered_site_data[l * 3 + 2];

              for (int kk = 0; kk < MACROSCOPIC_PARS; kk++)
              {
                snap << gathered_flow_field[MACROSCOPIC_PARS * l + kk];
              }
              snap << hemelb::io::Writer::eol;
            }
          }

          for (int l = 0; l < communication_period; l++)
          {
            local_site_data[l * 3] = -1;
          }

        } // weird for

      }

      if (mNetTopology->IsCurrentProcTheIOProc())
      {
        delete realSnap;
      }

      delete[] gathered_site_data;
      delete[] local_site_data;
      delete[] gathered_flow_field;
      delete[] local_flow_field;
    }

    void LBM::WriteConfigParallel(hemelb::lb::Stability stability,
                                  std::string output_file_name,
                                  const hemelb::lb::GlobalLatticeData &iGlobalLatticeData,
                                  const hemelb::lb::LocalLatticeData &iLocalLatticeData)
    {
      /* This routine writes the flow field on file. The data are gathered
       to the root processor and written from there.  The format
       comprises:

       0- Flag for simulation stability, 0 or 1

       1- Voxel size in physical units (units of m)

       2- vertex coords of the minimum bounding box with minimum values
       (x, y and z values)

       3- vertex coords of the minimum bounding box with maximum values
       (x, y and z values)

       4- #voxels within the minimum bounding box along the x, y, z axes
       (3 values)

       5- total number of fluid voxels

       6-And then a list of the fluid voxels... For each fluid voxel:

       a- the (x, y, z) coordinates in lattice units (3 values)
       b- the pressure in physical units (mmHg)
       c- (x,y,z) components of the velocity field in physical units (3
       values, m/s)
       d- the von Mises stress in physical units (Pa) (the stored shear
       stress is equal to -1 if the fluid voxel is not at the wall)
       */

      if (stability == hemelb::lb::Unstable)
      {
        MPI_File_delete(&output_file_name[0], MPI_INFO_NULL);
        return;
      }

      MPI_Status lStatus;

      MPI_File lOutputFile;

      MPI_File_open(MPI_COMM_WORLD, &output_file_name[0], MPI_MODE_WRONLY | MPI_MODE_CREATE,
                    MPI_INFO_NULL, &lOutputFile);

      /* Preamble has an enum (int) for stability, a double for voxel size,
       * 3 ints for minimum (x,y,z) in bounding box, 3 ints for maximum (x,y,z)
       * in bounding box, 3 ints for number of coords in each of (x,y,z),
       * 1 int for number of fluid voxels.*/
      const int lPreambleLength = 4 + 8 + (3 * 4) + (3 * 4) + (3 * 4) + 4;

      std::string lReadMode = "native";

      MPI_File_set_view(lOutputFile, 0, MPI_BYTE, MPI_BYTE, &lReadMode[0], MPI_INFO_NULL);

      if (mNetTopology->IsCurrentProcTheIOProc())
      {
        char lBuffer[lPreambleLength];
        hemelb::io::XdrMemWriter lWriter = hemelb::io::XdrMemWriter(lBuffer, lPreambleLength);

        lWriter << stability << voxel_size << site_min_x << site_min_y << site_min_z << site_max_x
            << site_max_y << site_max_z << (1 + site_max_x - site_min_x) << (1 + site_max_y
            - site_min_y) << (1 + site_max_z - site_min_z) << total_fluid_sites;

        MPI_File_write(lOutputFile, lBuffer, lPreambleLength, MPI_BYTE, &lStatus);
      }

      /*
       For each fluid voxel, we write
       a- the (x, y, z) coordinates in lattice units (3 ints)
       b- the pressure in physical units (mmHg, 1 x float)
       c- (x,y,z) components of the velocity field in physical units (3
       values, m/s, floats)
       d- the von Mises stress in physical units (Pa) (the stored shear
       stress is equal to -1 if the fluid voxel is not at the wall, 1 x float)
       */

      const int lOneFluidSiteLength = (3 * 4) + (5 * 4);

      int lLocalSitesInitialOffset = lPreambleLength;

      for (int ii = 0; ii < mNetTopology->GetLocalRank(); ii++)
      {
        lLocalSitesInitialOffset += lOneFluidSiteLength
            * mNetTopology->FluidSitesOnEachProcessor[ii];
      }

      MPI_File_set_view(lOutputFile, lLocalSitesInitialOffset, MPI_BYTE, MPI_BYTE, &lReadMode[0],
                        MPI_INFO_NULL);

      int lLocalWriteLength = lOneFluidSiteLength
          * mNetTopology->FluidSitesOnEachProcessor[mNetTopology->GetLocalRank()];
      char * lFluidSiteBuffer = new char[lLocalWriteLength];
      hemelb::io::XdrMemWriter lWriter = hemelb::io::XdrMemWriter(lFluidSiteBuffer,
                                                                  lLocalWriteLength);

      /* The following loops scan over every single macrocell (block). If
       the block is non-empty, it scans the fluid sites within that block
       If the site is fluid, it calculates the flow field and then is
       converted to physical units and stored in a buffer to send to the
       root processor */

      int n = -1; // net->proc_block counter
      for (int i = 0; i < iGlobalLatticeData.GetXSiteCount(); i
          += iGlobalLatticeData.GetBlockSize())
      {
        for (int j = 0; j < iGlobalLatticeData.GetYSiteCount(); j
            += iGlobalLatticeData.GetBlockSize())
        {
          for (int k = 0; k < iGlobalLatticeData.GetZSiteCount(); k
              += iGlobalLatticeData.GetBlockSize())
          {

            ++n;

            if (iGlobalLatticeData.Blocks[n].ProcessorRankForEachBlockSite == NULL)
            {
              continue;
            }
            int m = -1;

            for (int site_i = i; site_i < i + iGlobalLatticeData.GetBlockSize(); site_i++)
            {
              for (int site_j = j; site_j < j + iGlobalLatticeData.GetBlockSize(); site_j++)
              {
                for (int site_k = k; site_k < k + iGlobalLatticeData.GetBlockSize(); site_k++)
                {

                  m++;
                  if (mNetTopology->GetLocalRank()
                      != iGlobalLatticeData.Blocks[n].ProcessorRankForEachBlockSite[m])
                  {
                    continue;
                  }

                  unsigned int my_site_id = iGlobalLatticeData.Blocks[n].site_data[m];

                  /* No idea what this does */
                  if (my_site_id & (1U << 31U))
                    continue;

                  double density, vx, vy, vz, f_eq[D3Q15::NUMVECTORS], f_neq[D3Q15::NUMVECTORS],
                      stress, pressure;

                  // TODO Utter filth. The cases where the whole site data is exactly equal
                  // to "FLUID_TYPE" and where just the type-component of the whole site data
                  // is equal to "FLUID_TYPE" are handled differently.
                  if (iLocalLatticeData.mSiteData[my_site_id] == hemelb::lb::FLUID_TYPE)
                  {
                    D3Q15::CalculateDensityVelocityFEq(&iLocalLatticeData.FOld[my_site_id
                        * D3Q15::NUMVECTORS], density, vx, vy, vz, f_eq);

                    for (unsigned int l = 0; l < D3Q15::NUMVECTORS; l++)
                    {
                      f_neq[l] = iLocalLatticeData.FOld[my_site_id * D3Q15::NUMVECTORS + l]
                          - f_eq[l];
                    }

                  }
                  else
                  { // not FLUID_TYPE
                    CalculateBC(&iLocalLatticeData.FOld[my_site_id * D3Q15::NUMVECTORS],
                                iLocalLatticeData.GetSiteType(my_site_id),
                                iLocalLatticeData.GetBoundaryId(my_site_id), &density, &vx, &vy,
                                &vz, f_neq);
                  }

                  if (mParams.StressType == hemelb::lb::ShearStress)
                  {
                    if (iLocalLatticeData.GetNormalToWall(my_site_id)[0] >= BIG_NUMBER)
                    {
                      stress = -1.0;
                    }
                    else
                    {
                      D3Q15::CalculateShearStress(
                                                  density,
                                                  f_neq,
                                                  &iLocalLatticeData.GetNormalToWall(my_site_id)[0],
                                                  stress, mParams.StressParameter);
                    }
                  }
                  else
                  {
                    D3Q15::CalculateVonMisesStress(f_neq, stress, mParams.StressParameter);
                  }

                  vx /= density;
                  vy /= density;
                  vz /= density;

                  // conversion from lattice to physical units
                  pressure = ConvertPressureToPhysicalUnits(density * Cs2);

                  vx = ConvertVelocityToPhysicalUnits(vx);
                  vy = ConvertVelocityToPhysicalUnits(vy);
                  vz = ConvertVelocityToPhysicalUnits(vz);

                  stress = ConvertStressToPhysicalUnits(stress);

                  lWriter << (site_i - site_min_x) << (site_j - site_min_y)
                      << (site_k - site_min_z);

                  lWriter << float (pressure) << float (vx) << float (vy) << float (vz)
                      << float (stress);
                }
              }
            }
          }
        }
      }

      MPI_File_write_all(lOutputFile, lFluidSiteBuffer, lLocalWriteLength, MPI_BYTE, &lStatus);

      MPI_File_close(&lOutputFile);

      delete[] lFluidSiteBuffer;
    }

    void LBM::ReadVisParameters()
    {
      float lDensity_threshold_min, lDensity_threshold_minmax_inv, lVelocity_threshold_max_inv,
          lStress_threshold_max_inv;
      float par_to_send[9];
      float density_min, density_max, velocity_max, stress_max;

      int i;

      if (mNetTopology->IsCurrentProcTheIOProc())
      {
        velocity_max = ConvertVelocityToLatticeUnits(mSimConfig->MaxVelocity);
        stress_max = ConvertStressToLatticeUnits(mSimConfig->MaxStress);

        par_to_send[0] = mSimConfig->VisCentre.x;
        par_to_send[1] = mSimConfig->VisCentre.y;
        par_to_send[2] = mSimConfig->VisCentre.z;
        par_to_send[3] = mSimConfig->VisLongitude;
        par_to_send[4] = mSimConfig->VisLatitude;
        par_to_send[5] = mSimConfig->VisZoom;
        par_to_send[6] = mSimConfig->VisBrightness;
        par_to_send[7] = velocity_max;
        par_to_send[8] = stress_max;
      }

      int err = MPI_Bcast(par_to_send, 9, MPI_FLOAT, 0, MPI_COMM_WORLD);

      mSimConfig->VisCentre.x = par_to_send[0];
      mSimConfig->VisCentre.y = par_to_send[1];
      mSimConfig->VisCentre.z = par_to_send[2];
      mSimConfig->VisLongitude = par_to_send[3];
      mSimConfig->VisLatitude = par_to_send[4];
      mSimConfig->VisZoom = par_to_send[5];
      mSimConfig->VisBrightness = par_to_send[6];
      velocity_max = par_to_send[7];
      stress_max = par_to_send[8];

      density_min = ((float) BIG_NUMBER);
      density_max = ((float) -BIG_NUMBER);

      for (i = 0; i < inlets; i++)
      {
        density_min = fminf(density_min, inlet_density_avg[i] - inlet_density_amp[i]);
        density_max = fmaxf(density_max, inlet_density_avg[i] + inlet_density_amp[i]);
      }
      for (i = 0; i < outlets; i++)
      {
        density_min = fminf(density_min, outlet_density_avg[i] - outlet_density_amp[i]);
        density_max = fmaxf(density_max, outlet_density_avg[i] + outlet_density_amp[i]);
      }
      lDensity_threshold_min = density_min;

      lDensity_threshold_minmax_inv = 1.0F / (density_max - density_min);
      lVelocity_threshold_max_inv = 1.0F / velocity_max;
      lStress_threshold_max_inv = 1.0F / stress_max;

      hemelb::vis::controller->SetSomeParams(mSimConfig->VisBrightness, lDensity_threshold_min,
                                             lDensity_threshold_minmax_inv,
                                             lVelocity_threshold_max_inv, lStress_threshold_max_inv);
    }
  }
}