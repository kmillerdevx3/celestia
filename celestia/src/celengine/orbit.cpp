// orbit.cpp
//
// Copyright (C) 2001, Chris Laurel <claurel@shatters.net>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <functional>
#include <cmath>
#include <celmath/mathlib.h>
#include <celmath/solve.h>
#include "orbit.h"

using namespace std;


EllipticalOrbit::EllipticalOrbit(double _semiMajorAxis,
                                 double _eccentricity,
                                 double _inclination,
                                 double _ascendingNode,
                                 double _argOfPeriapsis,
                                 double _meanAnomalyAtEpoch,
                                 double _period,
                                 double _epoch) :
    semiMajorAxis(_semiMajorAxis),
    eccentricity(_eccentricity),
    inclination(_inclination),
    ascendingNode(_ascendingNode),
    argOfPeriapsis(_argOfPeriapsis),
    meanAnomalyAtEpoch(_meanAnomalyAtEpoch),
    period(_period),
    epoch(_epoch)
{
}


// First four terms of a series solution to Kepler's equation
// for orbital motion.  This only works for small eccentricities,
// and in fact the series diverges for e > 0.6627434
static double solveKeplerSeries(double M, double e)
{
    return (M +
            e * sin(M) +
            e * e * 0.5 * sin(2 * M) +
            e * e * e * (0.325 * sin(3 * M) - 0.125 * sin(M)) +
            e * e * e * e * (1.0 / 3.0 * sin(4 * M) - 1.0 / 6.0 * sin(2 * M)));

}


// Standard iteration for solving Kepler's Equation
struct SolveKeplerFunc1 : public unary_function<double, double>
{
    double ecc;
    double M;

    SolveKeplerFunc1(double _ecc, double _M) : ecc(_ecc), M(_M) {};

    double operator()(double x) const
    {
        return M + ecc * sin(x);
    }
};


// Faster converging iteration for Kepler's Equation; more efficient
// than above for orbits with eccentricities greater than 0.3.  This
// is from Jean Meeus's _Astronomical Algorithms_ (2nd ed), p. 199
struct SolveKeplerFunc2 : public unary_function<double, double>
{
    double ecc;
    double M;

    SolveKeplerFunc2(double _ecc, double _M) : ecc(_ecc), M(_M) {};

    double operator()(double x) const
    {
        return x + (M + ecc * sin(x) - x) / (1 - ecc * cos(x));
    }
};


typedef pair<double, double> Solution;

// Return the offset from the barycenter
Point3d EllipticalOrbit::positionAtTime(double t) const
{
    t = t - epoch;
    double meanMotion = 2.0 * PI / period;
    double meanAnomaly = meanAnomalyAtEpoch + t * meanMotion;

    // Compute the eccentric anomaly from the mean anomaly
    double eccAnomaly;
    if (eccentricity == 0.0)
    {
        // Circular orbit
        eccAnomaly = meanAnomaly;
    }
    else if (eccentricity < 0.2)
    {
        // Low eccentricity, so use the standard iteration technique
        Solution sol = solve_iteration_fixed(SolveKeplerFunc1(eccentricity,
                                                              meanAnomaly),
                                             meanAnomaly, 5);
        eccAnomaly = sol.first;
    }
    else if (eccentricity < 0.98)
    {
        // Higher eccentricity elliptical orbit; use a more complex but much
        // faster converging iteration.
        Solution sol = solve_iteration_fixed(SolveKeplerFunc2(eccentricity,
                                                              meanAnomaly),
                                             meanAnomaly, 6);
        eccAnomaly = sol.first;

        // Debugging
        // printf("ecc: %f, error: %f mas\n",
        //        eccentricity, radToDeg(sol.second) * 3600000);
    }
    else
    {
        // Need a technique for handling orbits that are nearly parabolic.
        eccAnomaly = meanAnomaly;
    }

    double x = semiMajorAxis * (cos(eccAnomaly) - eccentricity);
    double z = semiMajorAxis * sqrt(1 - square(eccentricity)) * -sin(eccAnomaly);

    Mat3d R = (Mat3d::yrotation(ascendingNode) *
               Mat3d::xrotation(inclination) *
               Mat3d::yrotation(argOfPeriapsis));

    return R * Point3d(x, 0, z);
}


double EllipticalOrbit::getPeriod() const
{
    return period;
}
