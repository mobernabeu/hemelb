#include "lb/collisions/ImplGuoZhengShi.h"

namespace hemelb
{
  namespace lb
  {
    namespace collisions
    {

      void ImplGuoZhengShi::DoCollisions(const bool iDoRayTracing,
                                         const int iFirstIndex,
                                         const int iSiteCount,
                                         const LbmParameters &iLbmParams,
                                         MinsAndMaxes &bMinimaAndMaxima,
                                         LocalLatticeData &bLocalLatDat,
                                         hemelb::vis::Control *iControl)
      {
        if (iDoRayTracing)
        {
          DoCollisionsInternal<true> (iFirstIndex, iSiteCount, iLbmParams,
                                      bMinimaAndMaxima, bLocalLatDat, iControl);
        }
        else
        {
          DoCollisionsInternal<false> (iFirstIndex, iSiteCount, iLbmParams,
                                       bMinimaAndMaxima, bLocalLatDat, iControl);
        }
      }

      template<bool tRayTracing>
      void ImplGuoZhengShi::DoCollisionsInternal(const int iFirstIndex,
                                                 const int iSiteCount,
                                                 const LbmParameters &iLbmParams,
                                                 MinsAndMaxes &bMinimaAndMaxima,
                                                 LocalLatticeData &bLocalLatDat,
                                                 hemelb::vis::Control *iControl)
      {
        for (int lIndex = iFirstIndex; lIndex < (iFirstIndex + iSiteCount); lIndex++)
        {

          // First do a normal collision & streaming step, as if we were mid-fluid.
          // NOTE that we use the version that preserves f_old.
          // NOTE that this handily works out the equilibrium density, v_x, v_y and v_z for us
          double lFEq[15];
          double f_neq[15];
          double density, v_x, v_y, v_z;
          double *f = &bLocalLatDat.FOld[lIndex * D3Q15::NUMVECTORS];

          D3Q15::CalculateDensityVelocityFEq(f, density, v_x, v_y, v_z, lFEq);

          for (unsigned int ii = 0; ii < D3Q15::NUMVECTORS; ii++)
          {
            bLocalLatDat.FNew[bLocalLatDat.GetStreamedIndex(lIndex, ii)]
                = f[ii] + iLbmParams.Omega * (f_neq[ii] = f[ii] - lFEq[ii]);
          }

          // Now fill in the un-streamed-to distributions (those that point away from boundaries).
          for (unsigned int l = 1; l < D3Q15::NUMVECTORS; l++)
          {
            int lAwayFromWallIndex = D3Q15::INVERSEDIRECTIONS[l];

            if (bLocalLatDat.HasBoundary(lIndex, l))
            {
              double delta = bLocalLatDat.GetCutDistance(lIndex, l);
              double uWall[3];
              double fNeq;

              // Work out uw1 (noting that ub is 0 until we implement moving walls)
              uWall[0] = (1 - 1. / delta) * v_x;
              uWall[1] = (1 - 1. / delta) * v_y;
              uWall[2] = (1 - 1. / delta) * v_z;
              fNeq = f_neq[lAwayFromWallIndex];

              // Interpolate with uw2 if delta < 0.75
              if (delta < 0.75)
              {
                // Only do the extra interpolation if there's gonna be a point there to interpolate from, i.e. there's no boundary
                // in the direction of awayFromWallIndex
                if (!bLocalLatDat.HasBoundary(lIndex, lAwayFromWallIndex))
                {
                  // Need some info about the next node away from the wall in this direction...
                  int nextIOut =
                      bLocalLatDat.GetStreamedIndex(lIndex, lAwayFromWallIndex)
                          / D3Q15::NUMVECTORS;
                  double nextNodeDensity, nextNodeV[3],
                      nextNodeFEq[D3Q15::NUMVECTORS];

                  D3Q15::CalculateDensityVelocityFEq(
                                                     &bLocalLatDat.FOld[nextIOut
                                                         * D3Q15::NUMVECTORS],
                                                     nextNodeDensity,
                                                     nextNodeV[0],
                                                     nextNodeV[1],
                                                     nextNodeV[2], nextNodeFEq);

                  for (int a = 0; a < 3; a++)
                    uWall[a] = delta * uWall[a] + (1. - delta) * (delta - 1.)
                        * nextNodeV[a] / (1. + delta);

                  fNeq = delta * fNeq + (1. - delta)
                      * (bLocalLatDat.FOld[nextIOut * D3Q15::NUMVECTORS
                          + lAwayFromWallIndex]
                          - nextNodeFEq[lAwayFromWallIndex]);
                }
                // If there's nothing to extrapolate from we, very lamely, do a 0VE-style operation to fill in the missing velocity.
                else
                {
                  for (int a = 0; a < 3; a++)
                    uWall[a] = 0.0;//delta * uWall[a];

                  fNeq = 0.0;//delta * fNeq;
                }
              }

              // Use a helper function to calculate the actual value of f_eq in the desired direction at the wall node.
              // Note that we assume that the density is the same as at this node
              double fEqTemp[D3Q15::NUMVECTORS];
              D3Q15::CalculateFeq(density, uWall[0], uWall[1], uWall[2],
                                  fEqTemp);

              // Collide and stream!
              bLocalLatDat.FNew[lIndex * D3Q15::NUMVECTORS + lAwayFromWallIndex]
                  = fEqTemp[lAwayFromWallIndex] + (1.0 + iLbmParams.Omega)
                      * fNeq;
            }
          }

          UpdateMinsAndMaxes<tRayTracing> (v_x, v_y, v_z, lIndex, f_neq,
                                           density, bMinimaAndMaxima,
                                           bLocalLatDat, iLbmParams, iControl);
        }
      }
    }
  }
}