//
// Copyright (C) 2014 OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/environment/ground/OsgEarthGround.h"


#include "inet/common/geometry/common/CoordinateSystem.h"
#include "inet/visualizer/scene/SceneOsgEarthVisualizer.h"
#include "inet/common/ModuleAccess.h"


namespace inet {

using namespace visualizer;

namespace physicalenvironment {

Define_Module(OsgEarthGround);

void OsgEarthGround::initialize(int stage)
{
    auto sceneVisualizer = getModuleFromPar<SceneOsgEarthVisualizer>(par("osgEarthSceneVisualizerModule"), this, false);
    map = sceneVisualizer->getMapNode()->getMap();
    elevationQuery = new osgEarth::ElevationQuery(map);
    coordinateSystem = getModuleFromPar<IGeographicCoordinateSystem>(par("coordinateSystemModule"), this);
}

double OsgEarthGround::getElevation(const Coord &position) const
{
    double elevation = 0;
    auto geoCoord = coordinateSystem->computeGeographicCoordinate(position);
    bool success = elevationQuery->getElevation(osgEarth::GeoPoint(map->getSRS(), geoCoord.longitude, geoCoord.latitude), elevation);

    return elevation;
}

} // namespace physicalenvironment

} // namespace inet

