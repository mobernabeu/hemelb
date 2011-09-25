#ifndef HEMELB_LB_RHEOLOGYMODELS_CASSONRHEOLOGYMODEL_H_
#define HEMELB_LB_RHEOLOGYMODELS_CASSONRHEOLOGYMODEL_H_

#include "lb/rheology_models/AbstractRheologyModel.h"

namespace hemelb
{
  namespace lb
  {
    namespace rheology_models
    {
      // Casson model constants
      static const double K0 = 0.1937; // Pa^{1/2}
      static const double K1 = 0.055; // (Pa*s)^{1/2}

      static const double CASSON_MAX_VISCOSITY = 0.16; // Pa*s

      class CassonRheologyModel : public AbstractRheologyModel<CassonRheologyModel>
      {
        public:
          /*
           *  Compute nu for a given shear rate according to the Casson model:
           *
           *  eta = (K0 + K1*sqrt(iShearRate))^2 / iShearRate
           *  nu = eta / density
           *
           *  @param iShearRate local shear rate value (s^{-1}).
           *  @param iDensity local density. TODO at the moment this value is not used
           *         in any subclass.
           *
           *  @return kinematic viscosity (m^2/s).
           */
          static double CalculateViscosityForShearRate(const double &iShearRate,
                                                       const distribn_t &iDensity);
      };
    }
  }
}

#endif /* HEMELB_LB_RHEOLOGYMODELS_CASSONRHEOLOGYMODEL_H_ */