// 
// Copyright (C) University College London, 2007-2012, all rights reserved.
// 
// This file is part of HemeLB and is CONFIDENTIAL. You may not work 
// with, install, use, duplicate, modify, redistribute or share this
// file, or any part thereof, other than as allowed by any agreement
// specifically made by you with University College London.
// 

#include "colloids/BoundaryConditions.h"
#include "colloids/LubricationBC.h"
#include "colloids/DeletionBC.h"
#include "geometry/Site.h"
#include "geometry/SiteData.h"

namespace hemelb
{
  namespace colloids
  {
    std::vector<BoundaryCondition*> BoundaryConditions::boundaryConditionsWall;
    std::vector<BoundaryCondition*> BoundaryConditions::boundaryConditionsInlet;
    std::vector<BoundaryCondition*> BoundaryConditions::boundaryConditionsOutlet;

    const geometry::LatticeData* BoundaryConditions::latticeData;

    const void BoundaryConditions::InitBoundaryConditions(
                                     const geometry::LatticeData* const latticeData,
                                     io::xml::XmlAbstractionLayer& xml)
    {
      BoundaryConditions::latticeData = latticeData;

      std::map<std::string, BoundaryConditionFactory_Create> mapBCGenerators;
      mapBCGenerators["LubricationBC"] = &(LubricationBoundaryConditionFactory::Create);
      mapBCGenerators["DeletionBC"] = &(DeletionBoundaryConditionFactory::Create);

      bool ok = true;
      xml.ResetToTopLevel();
      ok &= xml.MoveToChild("colloids");
      ok &= xml.MoveToChild("boundaryConditions");
      if (!ok) return;

      for (std::map<std::string, BoundaryConditionFactory_Create>::const_iterator
           iter = mapBCGenerators.begin();
           iter != mapBCGenerators.end();
           iter++)
      {
        const std::string boundaryConditionClass = iter->first;
        const BoundaryConditionFactory_Create createFunction = iter->second;
        bool found = xml.MoveToChild(boundaryConditionClass);
        if (found)
        {
          while (found)
          {
            std::string appliesTo;
            ok &= xml.GetString("appliesTo", appliesTo);
            BoundaryCondition* nextBC = createFunction(xml);
            if (appliesTo == "wall")
              BoundaryConditions::boundaryConditionsWall.push_back(nextBC);
            else if (appliesTo == "inlet")
              BoundaryConditions::boundaryConditionsInlet.push_back(nextBC);
            else if (appliesTo == "outlet")
              BoundaryConditions::boundaryConditionsOutlet.push_back(nextBC);
            found = xml.NextSibling(boundaryConditionClass);
          }
          xml.MoveToParent();
        }
      }
      xml.ResetToTopLevel();
    }

    const bool BoundaryConditions::DoSomeThingsToParticle(Particle& particle)
    {
      bool keep = true;

      // detect collision(s)
      const util::Vector3D<site_t> siteGlobalPosition(
        (site_t)(0.5+particle.GetGlobalPosition().x),
        (site_t)(0.5+particle.GetGlobalPosition().y),
        (site_t)(0.5+particle.GetGlobalPosition().z));
      proc_t procId;
      site_t localContiguousId;
      const bool isLocalFluid = latticeData->GetContiguousSiteId(
        siteGlobalPosition, procId, localContiguousId);
      if (particle.GetGlobalPosition().y < 1.5 && particle.GetGlobalPosition().y >= 0.5)
        printf("*** In BoundaryConditions::DoSomeThingsToParticle for id: %lu, p.y=%g, isLocalFluid: %s, procId: %u, localContiguousId: %lu, siteCoords: {%lu,%lu,%lu}, ownerRank: %u\n",
          particle.GetParticleId(),
          particle.GetGlobalPosition().y,
          isLocalFluid ? "TRUE" : "FALSE",
          procId, localContiguousId,
          siteGlobalPosition.x,
          siteGlobalPosition.y,
          siteGlobalPosition.z,
          particle.GetOwnerRank());

      if (!isLocalFluid) return !keep;

      const lb::lattices::LatticeInfo latticeInfo = BoundaryConditions::latticeData->GetLatticeInfo();
      const geometry::ConstSite site = latticeData->GetSite(localContiguousId);
      const geometry::SiteData siteData = site.GetSiteData();
      const geometry::SiteType siteType = siteData.GetSiteType();
      const distribn_t* siteWallDistances = site.GetWallDistances();

      const bool isNearWall = siteData.IsEdge();
      const bool isNearInlet = (siteType == geometry::INLET_TYPE);
      const bool isNearOutlet = (siteType == geometry::OUTLET_TYPE);

      // if the particle is not near a boundary then simply keep it
      if (!isNearWall && !isNearInlet && !isNearOutlet) return keep;
      else
        printf("*** In BoundaryConditions::DoSomeThingsToParticle for id: %lu, isNearWall: %s, isNearInlet: %s, isNearOutlet: %s ***\n",
          particle.GetParticleId(),
          isNearWall ? "TRUE" : "FALSE",
          isNearInlet ? "TRUE" : "FALSE",
          isNearOutlet ? "TRUE" : "FALSE");

      // only use lattice vectors 1 to 6 (the face-of-a-cube vectors)
      std::vector<LatticePosition> particleToWallVectors;
      for (Direction direction = 1; direction <= 6; ++direction)
      {
        // in general, this "distance" is a fraction of a non-unit lattice vector
        // however, we treat this fractional magnitude as a real lattice distance
        // because all of the face-of-a-cube lattice vectors will be unit vectors
        double thisDistance = siteWallDistances[direction - 1];

        // a negative distance to the wall from a site in any direction means that
        // the wall is further away than the nearest fluid site in that direction
        if (thisDistance < 0.0) continue;

        // the particle cannot be allowed to go past halfway between this site and
        // the next lattice site in this direction, because the next site is solid
        // the wall is assumed to be no further away than half the distance to the
        // solid site so that the particle never becomes nearest to a solid site
        if (thisDistance > 0.5) thisDistance = 0.5;

        const LatticePosition siteToWall = latticeInfo.GetVector(direction) * thisDistance;
        const LatticePosition particleToSite = siteGlobalPosition - particle.GetGlobalPosition();

        // particleToWall = siteToWall + projection of particleToSite in the siteToWall direction
        const LatticePosition particleToWallVector = siteToWall +
          siteToWall.GetNormalised() * siteToWall.GetNormalised().Dot(particleToSite);

        particleToWallVectors.push_back(particleToWallVector);
      }

      if (isNearWall)
        for (std::vector<BoundaryCondition*>::iterator iter = boundaryConditionsWall.begin();
             iter != boundaryConditionsWall.end(); iter++)
        {
          BoundaryCondition& boundaryCondition = **(iter);
          keep &= boundaryCondition.DoSomethingToParticle(particle, particleToWallVectors);
        }

      if (isNearInlet)
        for (std::vector<BoundaryCondition*>::iterator iter = boundaryConditionsInlet.begin();
             iter != boundaryConditionsInlet.end(); iter++)
        {
          BoundaryCondition& boundaryCondition = **(iter);
          keep &= boundaryCondition.DoSomethingToParticle(particle, particleToWallVectors);
        }

      if (isNearOutlet)
        for (std::vector<BoundaryCondition*>::iterator iter = boundaryConditionsOutlet.begin();
             iter != boundaryConditionsOutlet.end(); iter++)
        {
          BoundaryCondition& boundaryCondition = **(iter);
          keep &= boundaryCondition.DoSomethingToParticle(particle, particleToWallVectors);
        }

      return keep;
    }

  }
}