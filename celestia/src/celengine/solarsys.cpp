// solarsys.h
//
// Copyright (C) 2001 Chris Laurel <claurel@shatters.net>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <cassert>
// #include <limits>
#include <cstdio>

#ifndef _WIN32
#ifndef MACOSX_PB
#include <config.h>
#endif /* ! MACOSX_PB */
#endif /* ! _WIN32 */

#include <celutil/debug.h>
#include <celmath/mathlib.h>
#include <celutil/util.h>
#include <cstdio>
#include "astro.h"
#include "parser.h"
#include "customorbit.h"
#include "texmanager.h"
#include "meshmanager.h"
#include "trajmanager.h"
#include "universe.h"
#include "multitexture.h"

using namespace std;


static void errorMessagePrelude(const Tokenizer& tok)
{
    cerr << "Error in .ssc file (line " << tok.getLineNumber() << "): ";
}

static void sscError(const Tokenizer& tok,
                     const string& msg)
{
    errorMessagePrelude(tok);
    cerr << msg << '\n';
}


bool getDate(Hash* hash, const string& name, double& jd)
{
    // Check first for a number value representing a Julian date
    if (hash->getNumber(name, jd))
        return true;

    string dateString;
    if (hash->getString(name, dateString))
    {
        astro::Date date(1, 1, 1);
        if (astro::parseDate(dateString, date))
        {
            jd = (double) date;
            return true;
        }
    }

    return false;
}


static Location* CreateLocation(Hash* locationData,
                                Body* body)
{
    Location* location = new Location();

    Vec3d longlat(0.0, 0.0, 0.0);
    locationData->getVector("LongLat", longlat);

    Vec3f position = body->planetocentricToCartesian((float) longlat.x,
                                                     (float) longlat.y,
                                                     (float) longlat.z);
    location->setPosition(position);

    double size = 0.0;
    locationData->getNumber("Size", size);
    location->setSize((float) size);

    double importance = -1.0;
    locationData->getNumber("Importance", importance);
    location->setImportance((float) importance);

    string featureTypeName;
    if (locationData->getString("Type", featureTypeName))
        location->setFeatureType(Location::parseFeatureType(featureTypeName));

    return location;
}


static Surface* CreateSurface(Hash* surfaceData,
                              const std::string& path)
{
    Surface* surface = new Surface();

    surface->color = Color(1.0f, 1.0f, 1.0f);
    surfaceData->getColor("Color", surface->color);

    Color hazeColor;
    float hazeDensity = 0.0f;
    surfaceData->getColor("HazeColor", hazeColor);
    surfaceData->getNumber("HazeDensity", hazeDensity);
    surface->hazeColor = Color(hazeColor.red(), hazeColor.green(),
                               hazeColor.blue(), hazeDensity);

    surfaceData->getColor("SpecularColor", surface->specularColor);
    surfaceData->getNumber("SpecularPower", surface->specularPower);

    string baseTexture;
    string bumpTexture;
    string nightTexture;
    string specularTexture;
    string normalTexture;
    string overlayTexture;
    bool applyBaseTexture = surfaceData->getString("Texture", baseTexture);
    bool applyBumpMap = surfaceData->getString("BumpMap", bumpTexture);
    bool applyNightMap = surfaceData->getString("NightTexture", nightTexture);
    bool separateSpecular = surfaceData->getString("SpecularTexture",
                                                   specularTexture);
    bool applyNormalMap = surfaceData->getString("NormalMap", normalTexture);
    bool applyOverlay = surfaceData->getString("OverlayTexture",
                                               overlayTexture);

    unsigned int baseFlags = TextureInfo::WrapTexture | TextureInfo::AllowSplitting;
    unsigned int bumpFlags = TextureInfo::WrapTexture | TextureInfo::AllowSplitting;
    unsigned int nightFlags = TextureInfo::WrapTexture | TextureInfo::AllowSplitting;
    unsigned int specularFlags = TextureInfo::WrapTexture | TextureInfo::AllowSplitting;
    
    float bumpHeight = 2.5f;
    surfaceData->getNumber("BumpHeight", bumpHeight);

    bool blendTexture = false;
    surfaceData->getBoolean("BlendTexture", blendTexture);

    bool emissive = false;
    surfaceData->getBoolean("Emissive", emissive);

    bool compressTexture = false;
    surfaceData->getBoolean("CompressTexture", compressTexture);
    if (compressTexture)
        baseFlags |= TextureInfo::CompressTexture;

    if (blendTexture)
        surface->appearanceFlags |= Surface::BlendTexture;
    if (emissive)
        surface->appearanceFlags |= Surface::Emissive;
    if (applyBaseTexture)
        surface->appearanceFlags |= Surface::ApplyBaseTexture;
    if (applyBumpMap || applyNormalMap)
        surface->appearanceFlags |= Surface::ApplyBumpMap;
    if (applyNightMap)
        surface->appearanceFlags |= Surface::ApplyNightMap;
    if (separateSpecular)
        surface->appearanceFlags |= Surface::SeparateSpecularMap;
    if (applyOverlay)
        surface->appearanceFlags |= Surface::ApplyOverlay;
    if (surface->specularColor != Color(0.0f, 0.0f, 0.0f))
        surface->appearanceFlags |= Surface::SpecularReflection;

    if (applyBaseTexture)
        surface->baseTexture.setTexture(baseTexture, path, baseFlags);
    if (applyNightMap)
        surface->nightTexture.setTexture(nightTexture, path, nightFlags);
    if (separateSpecular)
        surface->specularTexture.setTexture(specularTexture, path, specularFlags);

    // If both are present, NormalMap overrides BumpMap
    if (applyNormalMap)
        surface->bumpTexture.setTexture(normalTexture, path, bumpFlags);
    else if (applyBumpMap)
        surface->bumpTexture.setTexture(bumpTexture, path, bumpHeight, bumpFlags);

    if (applyOverlay)
        surface->overlayTexture.setTexture(overlayTexture, path, baseFlags);

    return surface;
}


static EllipticalOrbit* CreateEllipticalOrbit(Hash* orbitData,
                                              bool usePlanetUnits)
{
    // SemiMajorAxis and Period are absolutely required; everything
    // else has a reasonable default.
    double pericenterDistance = 0.0;
    double semiMajorAxis = 0.0;
    if (!orbitData->getNumber("SemiMajorAxis", semiMajorAxis))
    {
        if (!orbitData->getNumber("PericenterDistance", pericenterDistance))
        {
            DPRINTF(0, "SemiMajorAxis/PericenterDistance missing!  Skipping planet . . .\n");
            return NULL;
        }
    }

    double period = 0.0;
    if (!orbitData->getNumber("Period", period))
    {
        DPRINTF(0, "Period missing!  Skipping planet . . .\n");
        return NULL;
    }

    double eccentricity = 0.0;
    orbitData->getNumber("Eccentricity", eccentricity);

    double inclination = 0.0;
    orbitData->getNumber("Inclination", inclination);

    double ascendingNode = 0.0;
    orbitData->getNumber("AscendingNode", ascendingNode);

    double argOfPericenter = 0.0;
    if (!orbitData->getNumber("ArgOfPericenter", argOfPericenter))
    {
        double longOfPericenter = 0.0;
        if (orbitData->getNumber("LongOfPericenter", longOfPericenter))
            argOfPericenter = longOfPericenter - ascendingNode;
    }

    double epoch = astro::J2000;
    getDate(orbitData, "Epoch", epoch);

    // Accept either the mean anomaly or mean longitude--use mean anomaly
    // if both are specified.
    double anomalyAtEpoch = 0.0;
    if (!orbitData->getNumber("MeanAnomaly", anomalyAtEpoch))
    {
        double longAtEpoch = 0.0;
        if (orbitData->getNumber("MeanLongitude", longAtEpoch))
            anomalyAtEpoch = longAtEpoch - (argOfPericenter + ascendingNode);
    }

    if (usePlanetUnits)
    {
        semiMajorAxis = astro::AUtoKilometers(semiMajorAxis);
        pericenterDistance = astro::AUtoKilometers(pericenterDistance);
        period = period * 365.25f;
    }

    // If we read the semi-major axis, use it to compute the pericenter
    // distance.
    if (semiMajorAxis != 0.0)
        pericenterDistance = semiMajorAxis * (1.0 - eccentricity);

    // cout << " bounding radius: " << semiMajorAxis * (1.0 + eccentricity) << "km\n";

    return new EllipticalOrbit(pericenterDistance,
                               eccentricity,
                               degToRad(inclination),
                               degToRad(ascendingNode),
                               degToRad(argOfPericenter),
                               degToRad(anomalyAtEpoch),
                               period,
                               epoch);
}


static RotationElements CreateRotationElements(Hash* rotationData,
                                               float orbitalPeriod)
{
    RotationElements re;

    // The default is synchronous rotation (rotation period == orbital period)
    float period = orbitalPeriod * 24.0f;
    rotationData->getNumber("RotationPeriod", period);
    re.period = period / 24.0f;

    float offset = 0.0f;
    rotationData->getNumber("RotationOffset", offset);
    re.offset = degToRad(offset);

    rotationData->getNumber("RotationEpoch", re.epoch);

    float obliquity = 0.0f;
    rotationData->getNumber("Obliquity", obliquity);
    re.obliquity = degToRad(obliquity);

    float ascendingNode = 0.0f;
    rotationData->getNumber("EquatorAscendingNode", ascendingNode);
    re.ascendingNode = degToRad(ascendingNode);

    float precessionRate = 0.0f;
    rotationData->getNumber("PrecessionRate", precessionRate);
    re.precessionRate = degToRad(precessionRate);

    return re;
}


// Create a body (planet or moon) using the values from a hash
// The usePlanetsUnits flags specifies whether period and semi-major axis
// are in years and AU rather than days and kilometers
static Body* CreatePlanet(PlanetarySystem* system,
                          Hash* planetData,
                          const string& path,
                          bool usePlanetUnits = true)
{
    Body* body = new Body(system);

    Orbit* orbit = NULL;
    string customOrbitName;

    if (planetData->getString("CustomOrbit", customOrbitName))
    {
        orbit = GetCustomOrbit(customOrbitName);
        if (orbit == NULL)
            DPRINTF(0, "Could not find custom orbit named '%s'\n",
                    customOrbitName.c_str());
    }

    if (orbit == NULL)
    {
        string sampOrbitFile;
        if (planetData->getString("SampledOrbit", sampOrbitFile))
        {
            DPRINTF(1, "Attempting to load sampled orbit file '%s'\n",
                    sampOrbitFile.c_str());
            ResourceHandle orbitHandle =
                GetTrajectoryManager()->getHandle(TrajectoryInfo(sampOrbitFile, path));
            orbit = GetTrajectoryManager()->find(orbitHandle);
            if (orbit == NULL)
            {
                DPRINTF(0, "Could not load sampled orbit file '%s'\n",
                        sampOrbitFile.c_str());
            }
        }
    }

    if (orbit == NULL)
    {
        Value* orbitDataValue = planetData->getValue("EllipticalOrbit");
        if (orbitDataValue != NULL)
        {
            if (orbitDataValue->getType() != Value::HashType)
            {
                DPRINTF(0, "Object '%s' has incorrect elliptical orbit syntax.\n",
                        body->getName().c_str());
            }
            else
            {
                orbit = CreateEllipticalOrbit(orbitDataValue->getHash(),
                                              usePlanetUnits);
            }
        }
    }
    if (orbit == NULL)
    {
        DPRINTF(0, "No valid orbit specified for object '%s'; skipping . . .\n",
                body->getName().c_str());
        delete body;
        return NULL;
    }
    body->setOrbit(orbit);

    double radius = 10000.0;
    planetData->getNumber("Radius", radius);
    body->setRadius(radius);

    int classification = Body::Unknown;
    string classificationName;
    if (planetData->getString("Class", classificationName))
    {
        if (compareIgnoringCase(classificationName, "planet") == 0)
            classification = Body::Planet;
        else if (compareIgnoringCase(classificationName, "moon") == 0)
            classification = Body::Moon;
        else if (compareIgnoringCase(classificationName, "comet") == 0)
            classification = Body::Comet;
        else if (compareIgnoringCase(classificationName, "asteroid") == 0)
            classification = Body::Asteroid;
        else if (compareIgnoringCase(classificationName, "spacecraft") == 0)
            classification = Body::Spacecraft;
        else if (compareIgnoringCase(classificationName, "invisible") == 0)
            classification = Body::Invisible;
        else
            classification = Body::Unknown;
    }

    if (classification == Body::Unknown)
    {
        //Try to guess the type
        if (system->getPrimaryBody() != NULL)
        {
            if(radius > 0.1)
                classification = Body::Moon;
            else
                classification = Body::Spacecraft;
        }
        else
        {
            if(radius < 1000.0)
                classification = Body::Asteroid;
            else
                classification = Body::Planet;
        }
    }
    body->setClassification(classification);

    // g++ is missing limits header, so we can use this
    // double beginning   = -numeric_limits<double>::infinity();
    // double ending      =  numeric_limits<double>::infinity();
    double beginning   = -1.0e+50;
    double ending      =  1.0e+50;
    getDate(planetData, "Beginning", beginning);
    getDate(planetData, "Ending", ending);
    body->setLifespan(beginning, ending);

    string infoURL;
    if (planetData->getString("InfoURL", infoURL))
        body->setInfoURL(infoURL);
    
    double albedo = 0.5;
    planetData->getNumber("Albedo", albedo);
    body->setAlbedo(albedo);

    double oblateness = 0.0;
    planetData->getNumber("Oblateness", oblateness);
    body->setOblateness(oblateness);

    Quatf orientation;
    if (planetData->getRotation("Orientation", orientation))
        body->setOrientation(orientation);

    body->setRotationElements(CreateRotationElements(planetData, orbit->getPeriod()));

    Surface* surface = CreateSurface(planetData, path);
    body->setSurface(*surface);
    delete surface;

    {
        string mesh("");
        if (planetData->getString("Mesh", mesh))
        {
            Vec3f meshCenter(0.0f, 0.0f, 0.0f);
            if (planetData->getVector("MeshCenter", meshCenter))
            {
                // TODO: Adjust bounding radius if mesh center isn't
                // (0.0f, 0.0f, 0.0f)
            }

            ResourceHandle meshHandle = GetMeshManager()->getHandle(MeshInfo(mesh, path, meshCenter));
            body->setMesh(meshHandle);

        }
    }

    // Read the atmosphere
    {
        Value* atmosDataValue = planetData->getValue("Atmosphere");
        if (atmosDataValue != NULL)
        {
            if (atmosDataValue->getType() != Value::HashType)
            {
                cout << "ReadSolarSystem: Atmosphere must be an assoc array.\n";
            }
            else
            {
                Hash* atmosData = atmosDataValue->getHash();
                assert(atmosData != NULL);
                
                Atmosphere* atmosphere = new Atmosphere();
                atmosData->getNumber("Height", atmosphere->height);
                atmosData->getColor("Lower", atmosphere->lowerColor);
                atmosData->getColor("Upper", atmosphere->upperColor);
                atmosData->getColor("Sky", atmosphere->skyColor);
                atmosData->getColor("Sunset", atmosphere->sunsetColor);
                atmosData->getNumber("CloudHeight", atmosphere->cloudHeight);
                atmosData->getNumber("CloudSpeed", atmosphere->cloudSpeed);
                atmosphere->cloudSpeed = degToRad(atmosphere->cloudSpeed);

                string cloudTexture;
                if (atmosData->getString("CloudMap", cloudTexture))
                {
                    atmosphere->cloudTexture.setTexture(cloudTexture,
                                                        path,
                                                        TextureInfo::WrapTexture);
                }

                body->setAtmosphere(*atmosphere);
                delete atmosphere;
            }

            delete atmosDataValue;
        }
    }

    // Read the ring system
    {
        Value* ringsDataValue = planetData->getValue("Rings");
        if (ringsDataValue != NULL)
        {
            if (ringsDataValue->getType() != Value::HashType)
            {
                cout << "ReadSolarSystem: Rings must be an assoc array.\n";
            }
            else
            {
                Hash* ringsData = ringsDataValue->getHash();
                // ASSERT(ringsData != NULL);

                double inner = 0.0, outer = 0.0;
                ringsData->getNumber("Inner", inner);
                ringsData->getNumber("Outer", outer);

                Color color(1.0f, 1.0f, 1.0f);
                ringsData->getColor("Color", color);

                string textureName;
                ringsData->getString("Texture", textureName);
                MultiResTexture ringTex(textureName, path);

                body->setRings(RingSystem((float) inner, (float) outer,
                                          color, ringTex));
            }

            delete ringsDataValue;
        }
    }

    return body;
}


bool LoadSolarSystemObjects(istream& in,
                            Universe& universe,
                            const std::string& directory)
{
    Tokenizer tokenizer(&in); 
    Parser parser(&tokenizer);

    while (tokenizer.nextToken() != Tokenizer::TokenEnd)
    {
        string itemType("Body");
        if (tokenizer.getTokenType() == Tokenizer::TokenName)
        {
            itemType = tokenizer.getNameValue();
            tokenizer.nextToken();
        }

        if (tokenizer.getTokenType() != Tokenizer::TokenString)
        {
            sscError(tokenizer, "object name expected");
            return false;
        }
        string name = tokenizer.getStringValue();

        if (tokenizer.nextToken() != Tokenizer::TokenString)
        {
            sscError(tokenizer, "bad parent object name");
            return false;
        }
        string parentName = tokenizer.getStringValue();

        Value* objectDataValue = parser.readValue();
        if (objectDataValue == NULL)
        {
            sscError(tokenizer, "bad object definition");
            return false;
        }

        if (objectDataValue->getType() != Value::HashType)
        {
            sscError(tokenizer, "{ expected");
            return false;
        }
        Hash* objectData = objectDataValue->getHash();

        Selection parent = universe.findPath(parentName, NULL, 0);
        PlanetarySystem* parentSystem = NULL;

        if (itemType == "Body")
        {
            bool orbitsPlanet = false;
            if (parent.star() != NULL)
            {
                SolarSystem* solarSystem = universe.getSolarSystem(parent.star());
                if (solarSystem == NULL)
                {
                    // No solar system defined for this star yet, so we need
                    // to create it.
                    solarSystem = universe.createSolarSystem(parent.star());
                }
                parentSystem = solarSystem->getPlanets();
            }
            else if (parent.body() != NULL)
            {
                // Parent is a planet or moon
                parentSystem = parent.body()->getSatellites();
                if (parentSystem == NULL)
                {
                    // If the planet doesn't already have any satellites, we
                    // have to create a new planetary system for it.
                    parentSystem = new PlanetarySystem(parent.body());
                    parent.body()->setSatellites(parentSystem);
                }
                orbitsPlanet = true;
            }
            else
            {
                errorMessagePrelude(tokenizer);
                cerr << "parent body '" << parentName << "' of '" << name << "' not found.\n";
            }

            if (parentSystem != NULL)
            {
                if (parentSystem->find(name))
                {
                    errorMessagePrelude(tokenizer);
                    cerr << "warning duplicate definition of " <<
                        parentName << " " <<  name << '\n';
                }
                
                Body* body = CreatePlanet(parentSystem, objectData, directory, !orbitsPlanet);
                if (body != NULL)
                {
                    body->setName(name);
                    parentSystem->addBody(body);
                }
            }
        }
        else if (itemType == "AltSurface")
        {
            Surface* surface = CreateSurface(objectData, directory);
            if (surface != NULL && parent.body() != NULL)
                parent.body()->addAlternateSurface(name, surface);
            else
                sscError(tokenizer, "bad alternate surface");
        }
        else if (itemType == "Location")
        {
            if (parent.body() != NULL)
            {
                Location* location = CreateLocation(objectData, parent.body());
                if (location != NULL)
                {
                    location->setName(name);
                    parent.body()->addLocation(location);
                }
                else
                {
                    sscError(tokenizer, "bad location");
                }
            }
            else
            {
                errorMessagePrelude(tokenizer);
                cerr << "parent body '" << parentName << "' of '" << name << "' not found.\n";
            }
        }
    }

    // TODO: Return some notification if there's an error parsing the file
    return true;
}


SolarSystem::SolarSystem(Star* _star) : star(_star)
{
    planets = new PlanetarySystem(_star);
}


Star* SolarSystem::getStar() const
{
    return star;
}

Point3f SolarSystem::getCenter() const
{
    // TODO: This is a very simple method at the moment, but it will get
    // more complex when planets around multistar systems are supported
    // where the planets may orbit the center of mass of two stars.
    return star->getPosition();
}

PlanetarySystem* SolarSystem::getPlanets() const
{
    return planets;
}
