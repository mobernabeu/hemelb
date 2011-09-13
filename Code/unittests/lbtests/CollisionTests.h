#ifndef HEMELB_UNITTESTS_LBTESTS_COLLISIONTESTS_H
#define HEMELB_UNITTESTS_LBTESTS_COLLISIONTESTS_H

#include <cppunit/TestFixture.h>

#include "SimConfig.h"
#include "lb/collisions/Collisions.h"
#include "lb/SimulationState.h"
#include "topology/NetworkTopology.h"
#include "unittests/TestLatticeData.h"
#include "unittests/TestSimConfig.h"
#include "util/UnitConverter.h"

namespace hemelb
{
  namespace unittests
  {
    namespace lbtests
    {
      /**
       * Class to test the collision operators. Note that we are only testing these here, so we
       * can assume that the kernel objects work perfectly.
       */
      class CollisionTests : public CppUnit::TestFixture
      {
        public:
          void setUp()
          {
            int args = 1;
            char** argv = NULL;
            bool success;
            topology::NetworkTopology::Instance()->Init(&args, &argv, &success);

            latDat = new TestLatticeData();
            simConfig = new TestSimConfig();
            simState = new lb::SimulationState(simConfig->StepsPerCycle, simConfig->NumCycles);
            lbmParams = new lb::LbmParameters(PULSATILE_PERIOD_s
                                                  / (distribn_t) simState->GetTimeStepsPerCycle(),
                                              latDat->GetVoxelSize());
            unitConverter = new util::UnitConverter(lbmParams, simState, latDat);

            inletBoundary = new lb::boundaries::BoundaryValues(geometry::LatticeData::INLET_TYPE,
                                                               latDat,
                                                               simConfig,
                                                               simState,
                                                               unitConverter);
            outletBoundary = new lb::boundaries::BoundaryValues(geometry::LatticeData::OUTLET_TYPE,
                                                                latDat,
                                                                simConfig,
                                                                simState,
                                                                unitConverter);

            lb::kernels::InitParams initParams;

            initParams.latDat = latDat;

            lbgk = new lb::kernels::LBGK(initParams);

            initParams.boundaryObject = inletBoundary;
            nonZeroVFixedDensityILet = new lb::collisions::NonZeroVelocityEquilibriumFixedDensity<
                lb::kernels::LBGK>(initParams);

            initParams.boundaryObject = outletBoundary;
            zeroVFixedDensityOLet = new lb::collisions::ZeroVelocityEquilibriumFixedDensity<
                lb::kernels::LBGK>(initParams);
            zeroVEqm = new lb::collisions::ZeroVelocityEquilibrium<lb::kernels::LBGK>(initParams);
            normal = new lb::collisions::Normal<lb::kernels::LBGK>(initParams);
          }

          void tearDown()
          {
            delete lbgk;
            delete nonZeroVFixedDensityILet;
            delete zeroVFixedDensityOLet;
            delete zeroVEqm;
            delete normal;

            delete latDat;
            delete simConfig;
            delete simState;
            delete lbmParams;
            delete unitConverter;
          }

          void TestNonZeroVelocityEquilibriumFixedDensity()
          {
            distribn_t allowedError = 1e-10;

            // Initialise the fOld and the hydro vars.
            distribn_t fOld[D3Q15::NUMVECTORS];

            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              fOld[ii] = ((distribn_t) (ii + 1)) / 10.0;
            }

            lb::kernels::HydroVars<lb::kernels::LBGK> hydroVars(fOld);

            // Test the pre-collision step, which should calculate the correct
            // post-collisional density, velocity and equilibrium distribution.
            nonZeroVFixedDensityILet->CalculatePreCollision(hydroVars, 0);

            // Calculate the expected density, velocity and f_eq.
            distribn_t expectedRho = inletBoundary->GetBoundaryDensity(0);
            distribn_t expectedV[3];

            KernelTestsHelper::CalculateVelocity<D3Q15>(fOld, expectedV);
            distribn_t expectedFeq[D3Q15::NUMVECTORS];
            KernelTestsHelper::CalculateLBGKEqmF<D3Q15>(expectedRho,
                                                        expectedV[0],
                                                        expectedV[1],
                                                        expectedV[2],
                                                        expectedFeq);

            // Compare.
            KernelTestsHelper::CompareHydros(expectedRho,
                                             expectedV[0],
                                             expectedV[1],
                                             expectedV[2],
                                             expectedFeq,
                                             "Non-0 velocity eqm fixed density, calculate pre collision",
                                             hydroVars,
                                             allowedError);

            // Next, compare the collision function itself. The result should be the equilibrium
            // distribution.
            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE("Non-0 velocity eqm fixed density, collide",
                                                   expectedFeq[ii],
                                                   nonZeroVFixedDensityILet->Collide(lbmParams,
                                                                                     ii,
                                                                                     hydroVars),
                                                   allowedError);
            }
          }

          void TestZeroVelocityEquilibriumFixedDensity()
          {
            distribn_t allowedError = 1e-10;

            // Initialise the fOld and the hydro vars.
            distribn_t fOld[D3Q15::NUMVECTORS];

            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              fOld[ii] = ((distribn_t) (ii + 1)) / 10.0;
            }

            lb::kernels::HydroVars<lb::kernels::LBGK> hydroVars(fOld);

            // Test the pre-collision step, which should calculate the correct
            // post-collisional density, velocity and equilibrium distribution.
            zeroVFixedDensityOLet->CalculatePreCollision(hydroVars, 0);

            // Calculate the expected density, velocity and f_eq.
            distribn_t expectedRho = outletBoundary->GetBoundaryDensity(0);
            distribn_t expectedV[3] = { 0., 0., 0. };

            distribn_t expectedFeq[D3Q15::NUMVECTORS];
            KernelTestsHelper::CalculateLBGKEqmF<D3Q15>(expectedRho,
                                                        expectedV[0],
                                                        expectedV[1],
                                                        expectedV[2],
                                                        expectedFeq);

            // Compare.
            KernelTestsHelper::CompareHydros(expectedRho,
                                             expectedV[0],
                                             expectedV[1],
                                             expectedV[2],
                                             expectedFeq,
                                             "0 velocity eqm fixed density, calculate pre collision",
                                             hydroVars,
                                             allowedError);

            // Next, compare the collision function itself. The result should be the equilibrium
            // distribution.
            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE("0 velocity eqm fixed density, collide",
                                                   expectedFeq[ii],
                                                   zeroVFixedDensityOLet->Collide(lbmParams,
                                                                                  ii,
                                                                                  hydroVars),
                                                   allowedError);
            }
          }

          void TestZeroVelocityEquilibrium()
          {
            distribn_t allowedError = 1e-10;

            // Initialise the fOld and the hydro vars.
            distribn_t fOld[D3Q15::NUMVECTORS];

            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              fOld[ii] = ((distribn_t) (ii + 1)) / 10.0;
            }

            lb::kernels::HydroVars<lb::kernels::LBGK> hydroVars(fOld);

            // Test the pre-collision step, which should calculate the correct
            // post-collisional density, velocity and equilibrium distribution.
            zeroVEqm->CalculatePreCollision(hydroVars, 0);

            // Calculate the expected density, velocity and f_eq.
            distribn_t expectedRho = 0.0;
            distribn_t expectedV[3] = { 0., 0., 0. };

            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              expectedRho += fOld[ii];
            }

            distribn_t expectedFeq[D3Q15::NUMVECTORS];
            KernelTestsHelper::CalculateLBGKEqmF<D3Q15>(expectedRho,
                                                        expectedV[0],
                                                        expectedV[1],
                                                        expectedV[2],
                                                        expectedFeq);

            // Compare.
            KernelTestsHelper::CompareHydros(expectedRho,
                                             expectedV[0],
                                             expectedV[1],
                                             expectedV[2],
                                             expectedFeq,
                                             "0 velocity eqm, calculate pre collision",
                                             hydroVars,
                                             allowedError);

            // Next, compare the collision function itself. The result should be the equilibrium
            // distribution.
            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE("0 velocity eqm, collide",
                                                   expectedFeq[ii],
                                                   zeroVEqm->Collide(lbmParams, ii, hydroVars),
                                                   allowedError);
            }
          }

          void TestNormal()
          {
            distribn_t allowedError = 1e-10;

            // Initialise the fOld and the hydro vars.
            distribn_t fOld[D3Q15::NUMVECTORS];

            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              fOld[ii] = ((distribn_t) (ii + 1)) / 10.0;
            }

            lb::kernels::HydroVars<lb::kernels::LBGK> hydroVars(fOld);

            // Test the pre-collision step, which should calculate the correct
            // post-collisional density, velocity and equilibrium distribution.
            normal->CalculatePreCollision(hydroVars, 0);

            // Calculate the expected density, velocity and f_eq.
            distribn_t expectedRho;
            distribn_t expectedV[3];

            KernelTestsHelper::CalculateRhoVelocity<D3Q15>(fOld, expectedRho, expectedV);

            distribn_t expectedFeq[D3Q15::NUMVECTORS];
            KernelTestsHelper::CalculateLBGKEqmF<D3Q15>(expectedRho,
                                                        expectedV[0],
                                                        expectedV[1],
                                                        expectedV[2],
                                                        expectedFeq);

            // Compare.
            KernelTestsHelper::CompareHydros(expectedRho,
                                             expectedV[0],
                                             expectedV[1],
                                             expectedV[2],
                                             expectedFeq,
                                             "Normal, calculate pre collision",
                                             hydroVars,
                                             allowedError);

            // Next, compare the collision function itself. The result should be the equilibrium
            // distribution.
            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              hydroVars.f_neq[ii] = hydroVars.f[ii] - hydroVars.f_eq[ii];
            }

            for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ++ii)
            {
              CPPUNIT_ASSERT_DOUBLES_EQUAL_MESSAGE("Normal, collide",
                                                   lbgk->Collide(lbmParams, hydroVars, ii),
                                                   normal->Collide(lbmParams, ii, hydroVars),
                                                   allowedError);
            }
          }

        private:
          geometry::LatticeData* latDat;
          SimConfig* simConfig;
          lb::SimulationState* simState;
          util::UnitConverter* unitConverter;
          lb::LbmParameters* lbmParams;

          lb::boundaries::BoundaryValues* inletBoundary;
          lb::boundaries::BoundaryValues* outletBoundary;

          lb::kernels::LBGK* lbgk;

          lb::collisions::NonZeroVelocityEquilibriumFixedDensity<lb::kernels::LBGK> * nonZeroVFixedDensityILet;
          lb::collisions::ZeroVelocityEquilibriumFixedDensity<lb::kernels::LBGK>* zeroVFixedDensityOLet;
          lb::collisions::ZeroVelocityEquilibrium<lb::kernels::LBGK>* zeroVEqm;
          lb::collisions::Normal<lb::kernels::LBGK>* normal;
      };
    }
  }
}

#endif /* HEMELB_UNITTESTS_LBTESTS_COLLISIONTESTS_H_ */
