// render.cpp
//
// Copyright (C) 2001, Chris Laurel <claurel@shatters.net>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cassert>

#ifndef _WIN32
#ifndef MACOSX_PB
#include <config.h>
#endif
#endif /* _WIN32 */

#include <celutil/debug.h>
#include <celmath/frustum.h>
#include <celmath/distance.h>
#include <celmath/intersect.h>
#include "gl.h"
#include "astro.h"
#include "glext.h"
#include "vecgl.h"
#include "spheremesh.h"
#include "lodspheremesh.h"
#include "model.h"
#include "regcombine.h"
#include "vertexprog.h"
#include "texmanager.h"
#include "meshmanager.h"
#include "rendcontext.h"
#include "render.h"

using namespace std;

#define FOV           45.0f
#define NEAR_DIST      0.5f
#define FAR_DIST   10000000.0f

// This should be in the GL headers, but where?
#ifndef GL_COLOR_SUM_EXT
#define GL_COLOR_SUM_EXT 0x8458
#endif

static const int StarVertexListSize = 1024;

// Fractional pixel offset used when rendering text as texture mapped
// quads to assure consistent mapping of texels to pixels.
static const float PixelOffset = 0.125f;

// These two values constrain the near and far planes of the view frustum
// when rendering planet and object meshes.  The near plane will never be
// closer than MinNearPlaneDistance, and the far plane is set so that far/near
// will not exceed MaxFarNearRatio.
static const float MinNearPlaneDistance = 0.0001f; // km
static const float MaxFarNearRatio      = 2000.0f;

static const float RenderDistance       = 50.0f;

static const float MaxScaledDiscStarSize = 8.0f;


// The minimum apparent size of an objects orbit in pixels before we display
// a label for it.  This minimizes label clutter.
static const float MinOrbitSizeForLabel = 20.0f;

// The minimum apparent size of a surface feature in pixels before we display
// a label for it.
static const float MinFeatureSizeForLabel = 20.0f;

// Static meshes and textures used by all instances of Simulation

static bool commonDataInitialized = false;

static LODSphereMesh* lodSphere = NULL;

static Texture* normalizationTex = NULL;

static Texture* starTex = NULL;
static Texture* glareTex = NULL;
static Texture* shadowTex = NULL;

// Shadow textures are scaled down slightly to leave some extra blank pixels
// near the border.  This keeps axis aligned streaks from appearing on hardware
// that doesn't support clamp to border color.
static const float ShadowTextureScale = 15.0f / 16.0f;

static Texture* eclipseShadowTextures[4];
static Texture* shadowMaskTexture = NULL;
static Texture* penumbraFunctionTexture = NULL;

static ResourceHandle starTexB = InvalidResource;
static ResourceHandle starTexA = InvalidResource;
static ResourceHandle starTexG = InvalidResource;
static ResourceHandle starTexM = InvalidResource;
static ResourceHandle starTexL = InvalidResource;

static const float CoronaHeight = 0.2f;

static bool buggyVertexProgramEmulation = true;

struct SphericalCoordLabel
{
    string label;
    float ra;
    float dec;

    SphericalCoordLabel() : ra(0), dec(0) {};
    SphericalCoordLabel(float _ra, float _dec) : ra(_ra), dec(_dec)
    {
    }
};

static int nCoordLabels = 32;
static SphericalCoordLabel* coordLabels = NULL;

static const int MaxSkyRings = 32;
static const int MaxSkySlices = 180;
static const int MinSkySlices = 30;


Renderer::Renderer() :
    context(0),
    windowWidth(0),
    windowHeight(0),
    fov(FOV),
    corrFac(1.12f),
    faintestAutoMag45deg(8.5f),
    renderMode(GL_FILL),
    labelMode(NoLabels),
    renderFlags(ShowStars | ShowPlanets),
    orbitMask(Body::Planet | Body::Moon),
    ambientLightLevel(0.1f),
    fragmentShaderEnabled(false),
    vertexShaderEnabled(false),
    brightnessBias(0.0f),
    saturationMagNight(1.0f),
    saturationMag(1.0f),
    starStyle(FuzzyPointStars),
    starVertexBuffer(NULL),
    useVertexPrograms(false),
    useRescaleNormal(false),
    useMinMaxBlending(false),
    textureResolution(medres),
    minOrbitSize(MinOrbitSizeForLabel),
    minFeatureSize(MinFeatureSizeForLabel),
    locationFilter(~0),
    distanceLimit(1.0e6f)
{
    starVertexBuffer = new StarVertexBuffer(2048);
    skyVertices = new SkyVertex[MaxSkySlices * (MaxSkyRings + 1)];
    skyIndices = new uint32[(MaxSkySlices + 1) * 2 * MaxSkyRings];
    skyContour = new SkyContourPoint[MaxSkySlices + 1];
}


Renderer::~Renderer()
{
    if (starVertexBuffer != NULL)
        delete starVertexBuffer;
    delete[] skyVertices;
    delete[] skyIndices;
    delete[] skyContour;
}


Renderer::DetailOptions::DetailOptions() :
    ringSystemSections(100),
    orbitPathSamplePoints(100),
    shadowTextureSize(256),
    eclipseTextureSize(128)
{
}


static void StarTextureEval(float u, float v, float w,
                            unsigned char *pixel)
{
    float r = 1 - (float) sqrt(u * u + v * v);
    if (r < 0)
        r = 0;
    else if (r < 0.5f)
        r = 2.0f * r;
    else 
        r = 1;

    int pixVal = (int) (r * 255.99f);
    pixel[0] = pixVal;
    pixel[1] = pixVal;
    pixel[2] = pixVal;
}

static void GlareTextureEval(float u, float v, float w,
                             unsigned char *pixel)
{
    float r = 0.9f - (float) sqrt(u * u + v * v);
    if (r < 0)
        r = 0;

    int pixVal = (int) (r * 255.99f);
    pixel[0] = 65;
    pixel[1] = 64;
    pixel[2] = 65;
    pixel[3] = pixVal;
}

static void ShadowTextureEval(float u, float v, float w,
                              unsigned char *pixel)
{
    float r = (float) sqrt(u * u + v * v);

    // Leave some white pixels around the edges to the shadow doesn't
    // 'leak'.  We'll also set the maximum mip map level for this texture to 3
    // so we don't have problems with the edge texels at high mip map levels.
    int pixVal = r < 15.0f / 16.0f ? 0 : 255;
    pixel[0] = pixVal;
    pixel[1] = pixVal;
    pixel[2] = pixVal;
}


//! Lookup function for eclipse penumbras--the input is the amount of overlap
//  between the occluder and sun disc, and the output is the fraction of
//  full brightness.
static void PenumbraFunctionEval(float u, float v, float w,
                                 unsigned char *pixel)
{
    u = (u + 1.0f) * 0.5f;

    // Using the cube root produces a good visual result
    unsigned char pixVal = (unsigned char) (::pow(u, 0.33) * 255.99);

    pixel[0] = pixVal;
}


// ShadowTextureFunction is a function object for creating shadow textures
// used for rendering eclipses.
class ShadowTextureFunction : public TexelFunctionObject
{
public:
    ShadowTextureFunction(float _umbra) : umbra(_umbra) {};
    virtual void operator()(float u, float v, float w, unsigned char* pixel);
    float umbra;
};

void ShadowTextureFunction::operator()(float u, float v, float w,
                                       unsigned char* pixel)
{
    float r = (float) sqrt(u * u + v * v);
    int pixVal = 255;

    // Leave some white pixels around the edges to the shadow doesn't
    // 'leak'.  We'll also set the maximum mip map level for this texture to 3
    // so we don't have problems with the edge texels at high mip map levels.
    r = r / (15.0f / 16.0f);
    if (r < 1)
    {
        // The pixel value should depend on the area of the sun which is
        // occluded.  We just fudge it here and use the square root of the
        // radius.
        if (r <= umbra)
            pixVal = 0;
        else
            pixVal = (int) (sqrt((r - umbra) / (1 - umbra)) * 255.99f);
    }

    pixel[0] = pixVal;
    pixel[1] = pixVal;
    pixel[2] = pixVal;
};


class ShadowMaskTextureFunction : public TexelFunctionObject
{
public:
    ShadowMaskTextureFunction() {};
    virtual void operator()(float u, float v, float w, unsigned char* pixel);
    float dummy;
};

void ShadowMaskTextureFunction::operator()(float u, float v, float w,
                                           unsigned char* pixel)
{
    unsigned char a = u > 0.0f ? 255 : 0;
    pixel[0] = a;
    pixel[1] = a;
    pixel[2] = a;
    pixel[3] = a;
}


static void IllumMapEval(float x, float y, float z,
                         unsigned char* pixel)
{
    Vec3f v(x, y, z);
    Vec3f u(0, 0, 1);

#if 0
    Vec3f n(0, 0, 1);
    // Experimental illumination function
    float c = v * n;
    if (c < 0.0f)
    {
        u = v;
    }
    else
    {
        c = (1 - ((1 - c))) * 1.0f;
        u = v + (c * n);
        u.normalize();
    }
#else
    u = v;
#endif

    pixel[0] = 128 + (int) (127 * u.x);
    pixel[1] = 128 + (int) (127 * u.y);
    pixel[2] = 128 + (int) (127 * u.z);
}

bool operator<(const RenderListEntry& a, const RenderListEntry& b)
{
    // This comparison functions tries to determine which of two objects is
    // closer to the viewer.  Looking just at the distances of the centers
    // is not enough, nor is comparing distances to the bounding spheres.
    // Here we trace a ray from the viewer to the center of the smaller
    // of the two objects and see which object it intersects first.  If the
    // ray doesn't intersect the larger object at all, it's safe to use
    // the distance to bounding sphere test.
    if (a.radius < b.radius)
    {
        Vec3f dir = a.position - Point3f(0.0f, 0.0f, 0.0f);
        float distance;
        dir.normalize();
        if (testIntersection(Ray3f(Point3f(0.0f, 0.0f, 0.0f), dir),
                             Spheref(b.position, b.radius),
                             distance))
        {
            return a.distance - a.radius < distance;
        }
    }
    else
    {
        Vec3f dir = b.position - Point3f(0.0f, 0.0f, 0.0f);
        float distance;
        dir.normalize();
        if (testIntersection(Ray3f(Point3f(0.0f, 0.0f, 0.0f), dir),
                             Spheref(a.position, a.radius),
                             distance))
        {
            return distance < b.distance - b.radius;
        }
    }

    return a.distance - a.radius < b.distance - b.radius;
}


bool operator<(const Renderer::Label& a, const Renderer::Label& b)
{
    return a.position.z > b.position.z;
}


bool Renderer::init(GLContext* _context,
                    int winWidth, int winHeight,
                    DetailOptions& _detailOptions)
{
    context = _context;
    detailOptions = _detailOptions;

    // Initialize static meshes and textures common to all instances of Renderer
    if (!commonDataInitialized)
    {
        lodSphere = new LODSphereMesh();

        starTex = CreateProceduralTexture(64, 64, GL_RGB, StarTextureEval);

        glareTex = LoadTextureFromFile("textures/flare.jpg");
        if (glareTex == NULL)
            glareTex = CreateProceduralTexture(64, 64, GL_RGB, GlareTextureEval);

        // Max mipmap level doesn't work reliably on all graphics
        // cards.  In particular, Rage 128 and TNT cards resort to software
        // rendering when this feature is enabled.  The only workaround is to
        // disable mipmapping completely unless texture border clamping is
        // supported, which solves the problem much more elegantly than all
        // the mipmap level nonsense.
        // shadowTex->setMaxMipMapLevel(3);
        Texture::AddressMode shadowTexAddress = Texture::EdgeClamp;
        Texture::MipMapMode shadowTexMip = Texture::NoMipMaps;
        useClampToBorder = context->extensionSupported("GL_ARB_texture_border_clamp");
        if (useClampToBorder)
        {
            shadowTexAddress = Texture::BorderClamp;
            shadowTexMip = Texture::DefaultMipMaps;
        }
        
        shadowTex = CreateProceduralTexture(detailOptions.shadowTextureSize,
                                            detailOptions.shadowTextureSize,
                                            GL_RGB,
                                            ShadowTextureEval,
                                            shadowTexAddress, shadowTexMip);
        shadowTex->setBorderColor(Color::White);

        // Create the eclipse shadow textures
        {
            for (int i = 0; i < 4; i++)
            {
                ShadowTextureFunction func(i * 0.25f);
                eclipseShadowTextures[i] =
                    CreateProceduralTexture(detailOptions.eclipseTextureSize,
                                            detailOptions.eclipseTextureSize,
                                            GL_RGB, func,
                                            shadowTexAddress, shadowTexMip);
                if (eclipseShadowTextures[i] != NULL)
                {
                    // eclipseShadowTextures[i]->setMaxMipMapLevel(2);
                    eclipseShadowTextures[i]->setBorderColor(Color::White);
                }
            }
        }

        // Create the shadow mask texture
        {
            ShadowMaskTextureFunction func;
            shadowMaskTexture = CreateProceduralTexture(128, 2, GL_RGBA, func);
            //shadowMaskTexture->bindName();
        }

        // Create a function lookup table in a texture for use with
        // fragment program eclipse shadows.
        penumbraFunctionTexture = CreateProceduralTexture(512, 1, GL_LUMINANCE,
                                                          PenumbraFunctionEval,
                                                          Texture::EdgeClamp);

        starTexB = GetTextureManager()->getHandle(TextureInfo("bstar.jpg", 0));
        starTexA = GetTextureManager()->getHandle(TextureInfo("astar.jpg", 0));
        starTexG = GetTextureManager()->getHandle(TextureInfo("gstar.jpg", 0));
        starTexM = GetTextureManager()->getHandle(TextureInfo("mstar.jpg", 0));
        starTexL = GetTextureManager()->getHandle(TextureInfo("browndwarf.jpg", 0));

        if (context->extensionSupported("GL_EXT_texture_cube_map"))
        {
            // normalizationTex = CreateNormalizationCubeMap(64);
            normalizationTex = CreateProceduralCubeMap(64, GL_RGB, IllumMapEval);
        }

        // Create labels for celestial sphere
        {
            char buf[10];
            int i;

            coordLabels = new SphericalCoordLabel[nCoordLabels];
            for (i = 0; i < 12; i++)
            {
                coordLabels[i].ra = float(i * 2);
                coordLabels[i].dec = 0;
                sprintf(buf, "%dh", i * 2);
                coordLabels[i].label = string(buf);
            }

            coordLabels[12] = SphericalCoordLabel(0, -75);
            coordLabels[13] = SphericalCoordLabel(0, -60);
            coordLabels[14] = SphericalCoordLabel(0, -45);
            coordLabels[15] = SphericalCoordLabel(0, -30);
            coordLabels[16] = SphericalCoordLabel(0, -15);
            coordLabels[17] = SphericalCoordLabel(0,  15);
            coordLabels[18] = SphericalCoordLabel(0,  30);
            coordLabels[19] = SphericalCoordLabel(0,  45);
            coordLabels[20] = SphericalCoordLabel(0,  60);
            coordLabels[21] = SphericalCoordLabel(0,  75);
            for (i = 22; i < nCoordLabels; i++)
            {
                coordLabels[i].ra = 12;
                coordLabels[i].dec = coordLabels[i - 10].dec;
            }

            for (i = 12; i < nCoordLabels; i++)
            {
                char buf[10];
                sprintf(buf, "%d", (int) coordLabels[i].dec);
                coordLabels[i].label = string(buf);
            }
        }

        commonDataInitialized = true;
    }

    if (context->extensionSupported("GL_EXT_rescale_normal"))
    {
        // We need this enabled because we use glScale, but only
        // with uniform scale factors.
        DPRINTF(1, "Renderer: EXT_rescale_normal supported.\n");
        useRescaleNormal = true;
        glEnable(GL_RESCALE_NORMAL_EXT);
    }
    if (context->extensionSupported("GL_EXT_blend_minmax"))
    {
        DPRINTF(1, "Renderer: minmax blending supported.\n");
        useMinMaxBlending = true;
    }

    // Ugly renderer-specific bug workarounds follow . . .
    char* glRenderer = (char*) glGetString(GL_RENDERER);
    if (glRenderer != NULL)
    {
        // Fog is broken with vertex program emulation in most versions of
        // the GF 1 and 2 drivers; we need to detect this and disable
        // vertex programs which output fog coordinates
        if (strstr(glRenderer, "GeForce3") != NULL ||
            strstr(glRenderer, "GeForce4") != NULL)
        {
            buggyVertexProgramEmulation = false;
        }

        if (strstr(glRenderer, "Savage4") != NULL ||
            strstr(glRenderer, "ProSavage") != NULL)
        {
            // S3 Savage4 drivers appear to rescale normals without reporting
            // EXT_rescale_normal.  Lighting will be messed up unless
            // we set the useRescaleNormal flag.
            useRescaleNormal = true;
        }
    }

    // More ugly hacks; according to Matt Craighead at NVIDIA, an NVIDIA
    // OpenGL driver that reports version 1.3.1 or greater will have working
    // fog in emulated vertex programs.
    char* glVersion = (char*) glGetString(GL_VERSION);
    if (glVersion != NULL)
    {
        int major = 0, minor = 0, extra = 0;
        int nScanned = sscanf(glVersion, "%d.%d.%d", &major, &minor, &extra);

        if (nScanned >= 2)
        {
            if (major > 1 || minor > 3 || (minor == 3 && extra >= 1))
                buggyVertexProgramEmulation = false;
        }
    }

    glLoadIdentity();

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_LIGHTING);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);

    // LEQUAL rather than LESS required for multipass rendering
    glDepthFunc(GL_LEQUAL);

    resize(winWidth, winHeight);

    return true;
}


void Renderer::resize(int width, int height)
{
    windowWidth = width;
    windowHeight = height;
    // glViewport(windowWidth, windowHeight);
}

float Renderer::calcPixelSize(float fovY, float windowHeight)
{
    return 2 * (float) tan(degToRad(fovY / 2.0)) / (float) windowHeight;
}

void Renderer::setFieldOfView(float _fov)
{
    fov = _fov;
    corrFac = (0.12f * fov/FOV * fov/FOV + 1.0f);
}

void Renderer::setFaintestAM45deg(float _faintestAutoMag45deg)
{
    faintestAutoMag45deg = _faintestAutoMag45deg;
}

float Renderer::getFaintestAM45deg()
{
    return faintestAutoMag45deg;
}

unsigned int Renderer::getResolution()
{
    return textureResolution;
}


void Renderer::setResolution(unsigned int resolution)
{
    if (resolution < TEXTURE_RESOLUTION)
        textureResolution = resolution;
}


TextureFont* Renderer::getFont() const
{
    return font;
}

void Renderer::setFont(TextureFont* txf)
{
    font = txf;
}

void Renderer::setRenderMode(int _renderMode)
{
    renderMode = _renderMode;
}

int Renderer::getRenderFlags() const
{
    return renderFlags;
}

void Renderer::setRenderFlags(int _renderFlags)
{
    renderFlags = _renderFlags;
}

int Renderer::getLabelMode() const
{
    return labelMode;
}

void Renderer::setLabelMode(int _labelMode)
{
    labelMode = _labelMode;
}

int Renderer::getOrbitMask() const
{
    return orbitMask;
}

void Renderer::setOrbitMask(int mask)
{
    orbitMask = mask;
}

void Renderer::addLabelledStar(Star* star, const string& label)
{
    labelledStars.insert(labelledStars.end(), StarLabel(star, label));
}


void Renderer::clearLabelledStars()
{
    labelledStars.clear();
}


float Renderer::getAmbientLightLevel() const
{
    return ambientLightLevel;
}


void Renderer::setAmbientLightLevel(float level)
{
    ambientLightLevel = level;
}


float Renderer::getMinimumFeatureSize() const
{
    return minFeatureSize;
}


void Renderer::setMinimumFeatureSize(float pixels)
{
    minFeatureSize = pixels;
}


float Renderer::getMinimumOrbitSize() const
{
    return minOrbitSize;
}


// Orbits and labels are only rendered when the orbit of the object
// occupies some minimum number of pixels on screen.
void Renderer::setMinimumOrbitSize(float pixels)
{
    minOrbitSize = pixels;
}


float Renderer::getDistanceLimit() const
{
    return distanceLimit;
}


void Renderer::setDistanceLimit(float distanceLimit_)
{
    distanceLimit = distanceLimit_;
}


bool Renderer::getFragmentShaderEnabled() const
{
    return fragmentShaderEnabled;
}

void Renderer::setFragmentShaderEnabled(bool enable)
{
    fragmentShaderEnabled = enable && fragmentShaderSupported();
}

bool Renderer::fragmentShaderSupported() const
{
    return context->bumpMappingSupported();
}

bool Renderer::getVertexShaderEnabled() const
{
    return vertexShaderEnabled;
}

void Renderer::setVertexShaderEnabled(bool enable)
{
    vertexShaderEnabled = enable && vertexShaderSupported();
}

bool Renderer::vertexShaderSupported() const
{
    return useVertexPrograms;
}


void Renderer::addLabel(string text,
                        Color color,
                        const Point3f& pos,
                        float depth)
{
    double winX, winY, winZ;
    int view[4] = { 0, 0, 0, 0 };
    view[0] = -windowWidth / 2;
    view[1] = -windowHeight / 2;
    view[2] = windowWidth;
    view[3] = windowHeight;
    depth = (float) (pos.x * modelMatrix[2] +
                     pos.y * modelMatrix[6] +
                     pos.z * modelMatrix[10]);
    if (gluProject(pos.x, pos.y, pos.z,
                   modelMatrix,
                   projMatrix,
                   (const GLint*) view,
                   &winX, &winY, &winZ) != GL_FALSE)
    {
        Label l;
        l.text = text;
        l.color = color;
        l.position = Point3f((float) winX, (float) winY, -depth);
        labels.insert(labels.end(), l);
    }
}


void Renderer::addSortedLabel(string text, Color color, const Point3f& pos)
{
    double winX, winY, winZ;
    int view[4] = { 0, 0, 0, 0 };
    view[0] = -windowWidth / 2;
    view[1] = -windowHeight / 2;
    view[2] = windowWidth;
    view[3] = windowHeight;
    float depth = (float) (pos.x * modelMatrix[2] +
                           pos.y * modelMatrix[6] +
                           pos.z * modelMatrix[10]);
    if (gluProject(pos.x, pos.y, pos.z,
                   modelMatrix,
                   projMatrix,
                   (const GLint*) view,
                   &winX, &winY, &winZ) != GL_FALSE)
    {
        Label l;
        l.text = text;
        l.color = color;
        l.position = Point3f((float) winX, (float) winY, -depth);
        depthSortedLabels.insert(depthSortedLabels.end(), l);
    }
}


void Renderer::clearLabels()
{
    labels.clear();
    depthSortedLabels.clear();
}


static void enableSmoothLines()
{
    // glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glLineWidth(1.5f);
}

static void disableSmoothLines()
{
    // glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_LINE_SMOOTH);
    glLineWidth(1.0f);
}


class OrbitRenderer : public OrbitSampleProc
{
public:
    OrbitRenderer() {};
    
    void sample(double, const Point3d& p)
    {
        glVertex3f(astro::kilometersToAU((float) p.x * 100),
                   astro::kilometersToAU((float) p.y * 100),
                   astro::kilometersToAU((float) p.z * 100));
        
    };

private:
    int dummy;
};


class OrbitSampler : public OrbitSampleProc
{
public:
    vector<Renderer::OrbitSample>* samples;

    OrbitSampler(vector<Renderer::OrbitSample>* _samples) : samples(_samples) {};
    void sample(double t, const Point3d& p)
    {
        Renderer::OrbitSample samp;
        samp.pos = Point3f((float) p.x, (float) p.y, (float) p.z);
        samp.t = t;
        samples->insert(samples->end(), samp);
    };
};



void Renderer::renderOrbit(Body* body, double t)
{
    vector<OrbitSample>* trajectory = NULL;
    for (vector<CachedOrbit*>::const_iterator iter = orbitCache.begin();
         iter != orbitCache.end(); iter++)
    {
        if ((*iter)->body == body)
        {
            (*iter)->keep = true;
            trajectory = &((*iter)->trajectory);
            break;
        }
    }

    // If it's not in the cache already
    if (trajectory == NULL)
    {
        CachedOrbit* orbit = NULL;

        // Search the cache an see if we can reuse an old orbit
        for (vector<CachedOrbit*>::const_iterator iter = orbitCache.begin();
             iter != orbitCache.end(); iter++)
        {
            if ((*iter)->body == NULL)
            {
                orbit = *iter;
                orbit->trajectory.clear();
                break;
            }
        }

        // If we can't reuse an old orbit, allocate a new one.
        bool reuse = true;
        if (orbit == NULL)
        {
            orbit = new CachedOrbit();
            reuse = false;
        }

        double startTime = t;
        int nSamples = detailOptions.orbitPathSamplePoints;

        // Adjust the number of samples used for aperiodic orbits--these aren't
        // true orbits, but are sampled trajectories, generally of spacecraft.
        // Better control is really needed--some sort of adaptive sampling would
        // be ideal.
        if (!body->getOrbit()->isPeriodic())
        {
            double begin = 0.0, end = 0.0;
            body->getOrbit()->getValidRange(begin, end);

            if (begin != end)
            {
                startTime = begin;
                nSamples = (int) (body->getOrbit()->getPeriod() * 100.0);
                nSamples = max(min(nSamples, 1000), 100);
            }
        }

        orbit->body = body;
        orbit->keep = true;
        OrbitSampler sampler(&orbit->trajectory);
        body->getOrbit()->sample(startTime,
                                 body->getOrbit()->getPeriod(),
                                 nSamples,
                                 sampler);
        trajectory = &orbit->trajectory;

        // If the orbit is new, put it back in the cache
        if (!reuse)
            orbitCache.insert(orbitCache.end(), orbit);
    }

    // Actually render the orbit
    if (body->getOrbit()->isPeriodic())
        glBegin(GL_LINE_LOOP);
    else
        glBegin(GL_LINE_STRIP);

    if ((renderFlags & ShowPartialTrajectories) == 0 ||
        body->getOrbit()->isPeriodic())
    {
        // Show the complete trajectory
        for (vector<OrbitSample>::const_iterator p = trajectory->begin();
             p != trajectory->end(); p++)
        {
            glVertex3f(astro::kilometersToAU(p->pos.x),
                       astro::kilometersToAU(p->pos.y),
                       astro::kilometersToAU(p->pos.z));
        }
    }
    else
    {
        vector<OrbitSample>::const_iterator p;

        // Show the portion of the trajectory travelled up to this point
        for (p = trajectory->begin(); p != trajectory->end() && p->t < t; p++)
        {
            glVertex3f(astro::kilometersToAU(p->pos.x),
                       astro::kilometersToAU(p->pos.y),
                       astro::kilometersToAU(p->pos.z));
        }

        // If we're midway through a non-periodic trajectory, we will need
        // to render a partial orbit segment.
        if (p != trajectory->end())
        {
            Point3d pos = body->getOrbit()->positionAtTime(t);
            glVertex3f((float) astro::kilometersToAU(pos.x),
                       (float) astro::kilometersToAU(pos.y),
                       (float) astro::kilometersToAU(pos.z));
        }
    }


    glEnd();
}


void Renderer::renderOrbits(PlanetarySystem* planets,
                            const Selection& sel,
                            double t,
                            const Point3d& observerPos,
                            const Point3d& center)
{
    if (planets == NULL)
        return;

    double distance = (center - observerPos).length();

    // At the solar system scale, we'll handle all calculations in AU
    // Not used anymore
    // Vec3d opos = (center - Point3d(0.0, 0.0, 0.0)) * astro::kilometersToAU(1.0);

    int nBodies = planets->getSystemSize();
    for (int i = 0; i < nBodies; i++)
    {
        Body* body = planets->getBody(i);
            
        // Only show orbits for major bodies or selected objects
        if ((body->getClassification() & orbitMask) != 0 || body == sel.body())
        {
            if (body == sel.body())
            {
                // Highlight the orbit of the selected object in red
                glColor4f(1, 0, 0, 1);
            }
            else
            {
                switch (body->getClassification())
                {
                case Body::Moon:
                    glColor4f(0.0f, 0.2f, 0.5f, 1.0f);
                    break;
                case Body::Asteroid:
                    glColor4f(0.35f, 0.2f, 0.0f, 1.0f);
                    break;
                case Body::Comet:
                    glColor4f(0.0f, 0.5f, 0.5f, 1.0f);
                    break;
                case Body::Spacecraft:
                    glColor4f(0.4f, 0.4f, 0.4f, 1.0f);
                    break;
                case Body::Planet:
                default:
                    glColor4f(0.0f, 0.4f, 1.0f, 1.0f);
                    break;
                }
            }
            
            float orbitRadiusInPixels =
                (float) (body->getOrbit()->getBoundingRadius() /
                         (distance * pixelSize));
            if (orbitRadiusInPixels > minOrbitSize)
            {
                float farDistance = 
                    (float) (body->getOrbit()->getBoundingRadius() + distance);
                farDistance = astro::kilometersToAU(farDistance);

                // Set up the projection matrix so that the far plane is
                // distant enough that the orbit won't be clipped.
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                gluPerspective(fov,
                               (float) windowWidth / (float) windowHeight,
                               max( farDistance * 1e-6f, (float)(1e-5/2./tan(fov*3.14159/360.0)) ),
                               farDistance * 1.1f );
                glMatrixMode(GL_MODELVIEW);
                renderOrbit(body, t);

                if (body->getSatellites() != NULL)
                {
                    Point3d localPos = body->getOrbit()->positionAtTime(t);
                    Quatd rotation =
                        Quatd::yrotation(body->getRotationElements().ascendingNode) *
                        Quatd::xrotation(body->getRotationElements().obliquity);
                    double scale = astro::kilometersToAU(1.0);
                    glPushMatrix();
                    glTranslated(localPos.x * scale,
                                 localPos.y * scale,
                                 localPos.z * scale);
                    glRotate(rotation);
                    renderOrbits(body->getSatellites(), sel, t,
                                 observerPos,
                                 body->getHeliocentricPosition(t));
                    glPopMatrix();
                }
            }
        }
    }
}

void Renderer::autoMag(float& faintestMag)
{
      float fieldCorr = 2.0f * FOV/(fov + FOV);
      faintestMag = (float) (faintestAutoMag45deg * sqrt(fieldCorr));
      saturationMag = saturationMagNight * (1.0f + fieldCorr * fieldCorr);
}


void Renderer::render(const Observer& observer,
                      const Universe& universe,
                      float faintestMagNight,
                      const Selection& sel)
{
    // Get the observer's time
    double now = observer.getTime();

    // Compute the size of a pixel
    setFieldOfView(radToDeg(observer.getFOV()));
    pixelSize = calcPixelSize(fov, (float) windowHeight);

    // Set up the projection we'll use for rendering stars.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(fov,
                   (float) windowWidth / (float) windowHeight,
                   NEAR_DIST, FAR_DIST);
    
    // Set the modelview matrix
    glMatrixMode(GL_MODELVIEW);

    // Get the displayed surface texture set to use from the observer
    displayedSurface = observer.getDisplayedSurface();

    locationFilter = observer.getLocationFilter();

    // Set up the camera
    Point3f observerPos = (Point3f) observer.getPosition();
    observerPos.x *= 1e-6f;
    observerPos.y *= 1e-6f;
    observerPos.z *= 1e-6f;
    glPushMatrix();
    glRotate(observer.getOrientation());

    // Get the model matrix *before* translation.  We'll use this for
    // positioning star and planet labels.
    glGetDoublev(GL_MODELVIEW_MATRIX, modelMatrix);
    glGetDoublev(GL_PROJECTION_MATRIX, projMatrix);
    
    clearLabels();

    // Put all solar system bodies into the render list.  Stars close and
    // large enough to have discernible surface detail are also placed in
    // renderList.
    renderList.clear();

    // See if there's a solar system nearby that we need to render.
    SolarSystem* solarSystem = universe.getNearestSolarSystem(observer.getPosition());
    const Star* sun = NULL;
    if (solarSystem != NULL)
        sun = solarSystem->getStar();

    // See if we want to use AutoMag.
    if ((renderFlags & ShowAutoMag) != 0)
    {
        autoMag(faintestMag);
    }
    else 
    {
        faintestMag = faintestMagNight;
        saturationMag = saturationMagNight;
    }

    // Automatic FOV-based adjustment of limiting magnitude for solar system
    // objects.  Now that we have the automag feature, this hack should no
    // longer be required.
    // faintestPlanetMag = faintestMag + (2.5f * (float) log10((double) square(45.0f / fov)));
    faintestPlanetMag = faintestMag;

    if ((sun != NULL) && ((renderFlags & ShowPlanets) != 0))
    {
        renderPlanetarySystem(*sun,
                              *solarSystem->getPlanets(),
                              observer,
                              now,
                              (labelMode & (BodyLabelMask)) != 0);
        starTex->bind();
    }

    Color skyColor(0.0f, 0.0f, 0.0f);

    // Scan through the render list to see if we're inside a planetary
    // atmosphere.  If so, we need to adjust the sky color as well as the
    // limiting magnitude of stars (so stars aren't visible in the daytime
    // on planets with thick atmospheres.)
    if ((renderFlags & ShowAtmospheres) != 0)
    {
        for (vector<RenderListEntry>::iterator iter = renderList.begin();
             iter != renderList.end(); iter++)
        {
            if (iter->body != NULL && iter->body->getAtmosphere() != NULL)
            {
                // Compute the density of the atmosphere, and from that
                // the amount light scattering.  It's complicated by the
                // possibility that the planet is oblate and a simple distance
                // to sphere calculation will not suffice.
                const Atmosphere* atmosphere = iter->body->getAtmosphere();
                float radius = iter->body->getRadius();
                float oblateness = iter->body->getOblateness();
                Vec3f recipSemiAxes(1.0f, 1.0f / (1.0f - oblateness), 1.0f);
                Mat3f A = Mat3f::scaling(recipSemiAxes);
                Vec3f eyeVec = iter->position - Point3f(0.0f, 0.0f, 0.0f);
                eyeVec *= (1.0f / radius);

                // Compute the orientation of the planet before axial rotation
                Quatd qd = iter->body->getEclipticalToEquatorial(now);
                Quatf q((float) qd.w, (float) qd.x, (float) qd.y, (float) qd.z);
                eyeVec = eyeVec * conjugate(q).toMatrix3();

                // ellipDist is not the true distance from the surface unless
                // the planet is spherical.  The quantity that we do compute
                // is the distance to the surface along a line from the eye
                // position to the center of the ellipsoid.
                float ellipDist = (float) sqrt((eyeVec * A) * (eyeVec * A)) - 1.0f;
                if (ellipDist < atmosphere->height / radius &&
                    atmosphere->height > 0.0f)
                {
                    float density = 1.0f - ellipDist /
                        (atmosphere->height / radius);
                    if (density > 1.0f)
                        density = 1.0f;

                    Vec3f sunDir = iter->sun;
                    Vec3f normal = Point3f(0.0f, 0.0f, 0.0f) - iter->position;
                    sunDir.normalize();
                    normal.normalize();
                    float illumination = Math<float>::clamp((sunDir * normal) + 0.2f);

                    float lightness = illumination * density;
                    faintestMag = faintestMag  - 15.0f * lightness;
                    saturationMag = saturationMag - 15.0f * lightness;
                }
            }
        }
    }

    // Now we need to determine how to scale the brightness of stars.  The
    // brightness will be proportional to the apparent magnitude, i.e.
    // a logarithmic function of the stars apparent brightness.  This mimics
    // the response of the human eye.  We sort of fudge things here and
    // maintain a minimum range of four magnitudes between faintest visible
    // and saturation; this keeps stars from popping in or out as the sun
    // sets or rises.
    if (faintestMag - saturationMag >= 4.0f)
        brightnessScale = 1.0f / (faintestMag -  saturationMag);
    else
        brightnessScale = 0.25f;

    ambientColor = Color(ambientLightLevel, ambientLightLevel, ambientLightLevel);

    // Create the ambient light source.  For realistic scenes in space, this
    // should be black.
    glAmbientLightColor(ambientColor);

    glClearColor(skyColor.red(), skyColor.green(), skyColor.blue(), 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glDisable(GL_LIGHTING);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);

    if ((renderFlags & ShowCelestialSphere) != 0)
    {
        glColor4f(0.3f, 0.7f, 0.7f, 0.55f);
        glDisable(GL_TEXTURE_2D);
        if ((renderFlags & ShowSmoothLines) != 0)
            enableSmoothLines();
        renderCelestialSphere(observer);
        if ((renderFlags & ShowSmoothLines) != 0)
            disableSmoothLines();
        glEnable(GL_BLEND);
        glEnable(GL_TEXTURE_2D);
    }

    if ((renderFlags & ShowGalaxies) != 0 &&
        universe.getDeepSkyCatalog() != NULL)
        renderDeepSkyObjects(*universe.getDeepSkyCatalog(), observer);

    // Translate the camera before rendering the stars
    glPushMatrix();
    glTranslatef(-observerPos.x, -observerPos.y, -observerPos.z);
    // Render stars
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    if ((renderFlags & ShowStars) != 0 && universe.getStarCatalog() != NULL)
        renderStars(*universe.getStarCatalog(), faintestMag, observer);

    // Render asterisms
    if ((renderFlags & ShowDiagrams) != 0 && universe.getAsterisms() != NULL)
    {
        glColor4f(0.28f, 0.0f, 0.66f, 0.96f);
        glDisable(GL_TEXTURE_2D);
        if ((renderFlags & ShowSmoothLines) != 0)
            enableSmoothLines();
        AsterismList* asterisms = universe.getAsterisms();
        for (AsterismList::const_iterator iter = asterisms->begin();
             iter != asterisms->end(); iter++)
        {
            Asterism* ast = *iter;

            for (int i = 0; i < ast->getChainCount(); i++)
            {
                const Asterism::Chain& chain = ast->getChain(i);

                glBegin(GL_LINE_STRIP);
                for (Asterism::Chain::const_iterator iter = chain.begin();
                     iter != chain.end(); iter++)
                    glVertex(*iter);
                glEnd();
            }
        }
        
        if ((renderFlags & ShowSmoothLines) != 0)
            disableSmoothLines();
    }

    if ((renderFlags & ShowBoundaries) != 0)
    {
        glColor4f(0.8f, 0.33f, 0.63f, 0.35f);
        glDisable(GL_TEXTURE_2D);
        if ((renderFlags & ShowSmoothLines) != 0)
            enableSmoothLines();
        if (universe.getBoundaries() != NULL)
            universe.getBoundaries()->render();
        if ((renderFlags & ShowSmoothLines) != 0)
            disableSmoothLines();
    }

    if ((labelMode & GalaxyLabels) != 0 && universe.getDeepSkyCatalog() != NULL)
        labelGalaxies(*universe.getDeepSkyCatalog(), observer);
    if ((labelMode & StarLabels) != 0 && universe.getStarCatalog() != NULL)
        labelStars(labelledStars, *universe.getStarCatalog(), observer);
    if ((labelMode & ConstellationLabels) != 0 &&
        universe.getAsterisms() != NULL)
        labelConstellations(*universe.getAsterisms(), observer);

    glPopMatrix();

    if ((renderFlags & ShowOrbits) != 0 && orbitMask != 0 && solarSystem != NULL)
    {
        vector<CachedOrbit*>::const_iterator iter;

        // Clear the keep flag for all orbits in the cache; if they're not
        // used when rendering this frame, they'll get marked for recycling.
        for (iter = orbitCache.begin(); iter != orbitCache.end(); iter++)
            (*iter)->keep = false;

        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        if ((renderFlags & ShowSmoothLines) != 0)
            enableSmoothLines();
        
        const Star* sun = solarSystem->getStar();
        Point3d obsPos = astro::heliocentricPosition(observer.getPosition(),
                                                     sun->getPosition());
        glPushMatrix();
        glTranslatef((float) astro::kilometersToAU(-obsPos.x),
                     (float) astro::kilometersToAU(-obsPos.y),
                     (float) astro::kilometersToAU(-obsPos.z));
        renderOrbits(solarSystem->getPlanets(), sel, now,
                     obsPos, Point3d(0.0, 0.0, 0.0));
        glPopMatrix();

        if ((renderFlags & ShowSmoothLines) != 0)
            disableSmoothLines();

        // Mark for recycling all unused orbits in the cache
        for (iter = orbitCache.begin(); iter != orbitCache.end(); iter++)
        {
            if (!(*iter)->keep)
                (*iter)->body = NULL;
        }

    }

    renderLabels();

    glPolygonMode(GL_FRONT, (GLenum) renderMode);
    glPolygonMode(GL_BACK, (GLenum) renderMode);

    {
        Frustum frustum(degToRad(fov),
                        (float) windowWidth / (float) windowHeight,
                        MinNearPlaneDistance);
        Mat3f viewMat = conjugate(observer.getOrientation()).toMatrix3();

        // Remove objects from the render list that lie completely outside the
        // view frustum.
        vector<RenderListEntry>::iterator notCulled = renderList.begin();
        for (vector<RenderListEntry>::iterator iter = renderList.begin();
             iter != renderList.end(); iter++)
        {
            Point3f center = iter->position * viewMat;

            bool convex = true;
            float radius = 1.0f;
            float cullRadius = 1.0f;
            float cloudHeight = 0.0f;
            if (iter->body != NULL)
            {
                radius = iter->body->getBoundingRadius();
                if (iter->body->getRings() != NULL)
                {
                    radius = iter->body->getRings()->outerRadius;
                    convex = false;
                }

                if (iter->body->getModel() != InvalidResource)
                    convex = false;

                cullRadius = radius;
                if (iter->body->getAtmosphere() != NULL)
                {
                    cullRadius += iter->body->getAtmosphere()->height;
                    cloudHeight = iter->body->getAtmosphere()->cloudHeight;
                }
            }
            else if (iter->star != NULL)
            {
                radius = iter->star->getRadius();
                cullRadius = radius * (1.0f + CoronaHeight);
            }

            // Test the object's bounding sphere against the view frustum
            if (frustum.testSphere(center, cullRadius) != Frustum::Outside)
            {
                float nearZ = center.distanceFromOrigin() - radius;
                float maxSpan = (float) sqrt(square((float) windowWidth) +
                                             square((float) windowHeight));

                nearZ = -nearZ * (float) cos(degToRad(fov / 2)) *
                    ((float) windowHeight / maxSpan);
                
                if (nearZ > -MinNearPlaneDistance)
                    iter->nearZ = -MinNearPlaneDistance;
                else
                    iter->nearZ = nearZ;

                if (!convex)
                {
                    iter->farZ = center.z - radius;
                    if (iter->farZ / iter->nearZ > MaxFarNearRatio)
                        iter->nearZ = iter->farZ / MaxFarNearRatio;
                }
                else
                {
                    // Make the far plane as close as possible
                    float d = center.distanceFromOrigin();

                    // Account for the oblateness
                    float eradius = radius;
                    if (iter->body != NULL)
                        eradius *= 1.0f - iter->body->getOblateness();

                    if (d > eradius)
                    {
                        // Multiply by a factor to eliminate overaggressive
                        // clipping due to limited floating point precision
                        iter->farZ = (float) (sqrt(square(d) - 
                                                   square(eradius)) * -1.1);
                    }
                    else
                    {
                        // We're inside the bounding sphere (and, if the planet
                        // is spherical, inside the planet.)
                        iter->farZ = iter->nearZ * 2.0f;
                    }

                    if (cloudHeight > 0.0f && d < eradius + cloudHeight)
                    {
                        // If there's a cloud layer, we need to move the 
                        // far plane out so that the clouds aren't clipped
                        float cloudLayerRadius = eradius + cloudHeight;
                        iter->farZ -= (float) sqrt(square(cloudLayerRadius) -
                                                   square(eradius));
                    }
                }

                *notCulled = *iter;
                notCulled++;
            }
        }

        renderList.resize(notCulled - renderList.begin());

        // The calls to renderSolarSystem/renderStars filled renderList
        // with visible planetary bodies.  Sort it by distance, then
        // render each entry.
        stable_sort(renderList.begin(), renderList.end());

        // Sort the labels
        sort(depthSortedLabels.begin(), depthSortedLabels.end());

        int nEntries = renderList.size();

        // Determine how to split up the depth buffer.  Typically, each body 
        // with an apparent size greater than one pixel is allocated its own
        // depth buffer range.  However, this will not correctly handle
        // overlapping objects.  If two objects overlap in depth, we attempt
        // to coalesce their depth buckets.  Coalescing will succeed as long
        // as the far / near plane ratio is not too large.  If it does exceed
        // the limit, coaslescing fails and the objects may not be rendered
        // correctly, though the result should be superior to throwing away
        // depth buffer precision by allowing the far / near ratio to get too
        // large.
        int nDepthBuckets = 1;
        int i;
        {
            float lastNear = 0.0f;
            float lastFar = 0.0f;
            int firstCoalesced = 0;

            for (i = 0; i < nEntries; i++)
            {
                // Don't worry about objects that are smaller than a pixel,
                // as they'll just be rendered as points.
                if (renderList[i].discSizeInPixels > 1)
                {
                    if (nDepthBuckets == 1 || renderList[i].nearZ <= lastFar)
                    {
                        // This object doesn't overlap any others in depth,
                        // no need to coaslesce.
                        nDepthBuckets++;
                        firstCoalesced = i;
                        lastNear = renderList[i].nearZ;
                        lastFar = renderList[i].farZ;
                    }
                    else
                    {
                        // Consider coalescing this object with the previous
                        // one.
                        float farthest = min(lastFar, renderList[i].farZ);
                        float nearest = max(lastNear, renderList[i].nearZ);

                        if (farthest / nearest < MaxFarNearRatio)
                        {
                            // Far/near ratio is acceptable, so coalesce
                            // this object's depth bucket with the previous
                            // one.
                            for (int j = firstCoalesced; j <= i; j++)
                            {
                                renderList[j].farZ  = farthest;
                                renderList[j].nearZ = nearest;
                            }
                        }
                        else
                        {
                            // Coalesce failed, so create a new depth bucket
                            // for this object.
                            nDepthBuckets++;
                            firstCoalesced = i;
                            lastNear = renderList[i].nearZ;
                            lastFar = renderList[i].farZ;
                        }
                    }
                    renderList[i].depthBucket = nDepthBuckets - 1;
                }
            }
        }
        
        float depthRange = 1.0f / (float) nDepthBuckets;

        int depthBucket = nDepthBuckets - 1;
        i = nEntries - 1;

        // Set up the depth bucket.
        glDepthRange(depthBucket * depthRange, (depthBucket + 1) * depthRange);

        // Set the initial near and far plane distance; any reasonable choice
        // for these will do, since different values will be chosen as soon
        // as we need to render a body as a mesh.
        float nearPlaneDistance = 1.0f;
        float farPlaneDistance = 10.0f;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(fov, (float) windowWidth / (float) windowHeight,
                       nearPlaneDistance, farPlaneDistance);
        glMatrixMode(GL_MODELVIEW);

        vector<Label>::iterator label = depthSortedLabels.begin();

        // Render all the bodies in the render list.
        for (i = nEntries - 1; i >= 0; i--)
        {
            label = renderSortedLabels(label, -renderList[i].farZ);

            if (renderList[i].discSizeInPixels > 1)
            {
                depthBucket = renderList[i].depthBucket;
                glDepthRange(depthBucket * depthRange, (depthBucket + 1) * depthRange);
                
                nearPlaneDistance = renderList[i].nearZ * -0.9f;
                farPlaneDistance = renderList[i].farZ * -1.5f;
                if (nearPlaneDistance < MinNearPlaneDistance)
                    nearPlaneDistance = MinNearPlaneDistance;
                if (farPlaneDistance / nearPlaneDistance > MaxFarNearRatio)
                    farPlaneDistance = nearPlaneDistance * MaxFarNearRatio;

                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                gluPerspective(fov, (float) windowWidth / (float) windowHeight,
                               nearPlaneDistance, farPlaneDistance);
                glMatrixMode(GL_MODELVIEW);
            }

            if (renderList[i].body != NULL)
            {
                if (renderList[i].isCometTail)
                {
                    renderCometTail(*renderList[i].body,
                                    renderList[i].position,
                                    renderList[i].sun,
                                    renderList[i].distance,
                                    renderList[i].appMag,
                                    now,
                                    observer.getOrientation(),
                                    nearPlaneDistance, farPlaneDistance);
                }
                else
                {
                    renderPlanet(*renderList[i].body,
                                 renderList[i].position,
                                 renderList[i].sun,
                                 renderList[i].distance,
                                 renderList[i].appMag,
                                 now,
                                 observer.getOrientation(),
                                 nearPlaneDistance, farPlaneDistance);
                }
            }
            else if (renderList[i].star != NULL)
            {
                renderStar(*renderList[i].star,
                           renderList[i].position,
                           renderList[i].distance,
                           renderList[i].appMag,
                           observer.getOrientation(),
                           now,
                           nearPlaneDistance, farPlaneDistance);
            }

            // If this body is larger than a pixel, we rendered it as a mesh
            // instead of just a particle.  We move to the next depth buffer 
            // bucket before proceeding with further rendering.
            if (renderList[i].discSizeInPixels > 1)
            {
                depthBucket--;
                glDepthRange(depthBucket * depthRange, (depthBucket + 1) * depthRange);
            }
        }

        renderSortedLabels(label, 0.0f);

        // reset the depth range
        glDepthRange(0, 1);
    }

    glPopMatrix();

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPolygonMode(GL_FRONT, GL_FILL);
    glPolygonMode(GL_BACK, GL_FILL);

    {
        Mat3f m = observer.getOrientation().toMatrix3();
        for (int i = 0; i < (int) labels.size(); i++)
        {
            
        }
    }

    if ((renderFlags & ShowMarkers) != 0)
    {
        renderMarkers(*universe.getMarkers(),
                      observer.getPosition(),
                      observer.getOrientation(),
                      now);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_LIGHTING);

#if 0
    int errCode = glGetError();
    if (errCode != GL_NO_ERROR)
    {
        cout << "glError: " << (char*) gluErrorString(errCode) << '\n';
    }
#endif
}


static void renderRingSystem(float innerRadius,
                             float outerRadius,
                             float beginAngle,
                             float endAngle,
                             unsigned int nSections)
{
    float angle = endAngle - beginAngle;

    glBegin(GL_QUAD_STRIP);
    for (unsigned int i = 0; i <= nSections; i++)
    {
        float t = (float) i / (float) nSections;
        float theta = beginAngle + t * angle;
        float s = (float) sin(theta);
        float c = (float) cos(theta);
        glTexCoord2f(0, 0.5f);
        glVertex3f(c * innerRadius, 0, s * innerRadius);
        glTexCoord2f(1, 0.5f);
        glVertex3f(c * outerRadius, 0, s * outerRadius);
    }
    glEnd();
}


// If the an object occupies a pixel or less of screen space, we don't
// render its mesh at all and just display a starlike point instead.
// Switching between the particle and mesh renderings of an object is
// jarring, however . . . so we'll blend in the particle view of the
// object to smooth things out, making it dimmer as the disc size exceeds the
// max disc size.
void Renderer::renderBodyAsParticle(Point3f position,
                                    float appMag,
                                    float _faintestMag,
                                    float discSizeInPixels,
                                    Color color,
                                    const Quatf& orientation,
                                    float renderZ,
                                    bool useHaloes)
{
    float maxDiscSize = (starStyle == ScaledDiscStars) ? MaxScaledDiscStarSize : 1.0f;
    float maxBlendDiscSize = maxDiscSize + 3.0f;
    float discSize = 1.0f;

    if (discSizeInPixels < maxBlendDiscSize || useHaloes)
    {
        float a = 1;
        float fade = 1.0f;

        if (discSizeInPixels > maxDiscSize)
        {
            fade = (maxBlendDiscSize - discSizeInPixels) /
                (maxBlendDiscSize - maxDiscSize - 1.0f);
            if (fade > 1)
                fade = 1;
        }

        a = (_faintestMag - appMag) * brightnessScale + brightnessBias;
        if (starStyle == ScaledDiscStars && a > 1.0f)
            discSize = min(discSize * (2.0f * a - 1.0f), maxDiscSize);
        a = clamp(a) * fade;

        // We scale up the particle by a factor of 1.5 (at fov = 45deg) 
        // so that it's more
        // visible--the texture we use has fuzzy edges, and if we render it
        // in just one pixel, it's likely to disappear.  Also, the render
        // distance is scaled by a factor of 0.1 so that we're rendering in
        // front of any mesh that happens to be sharing this depth bucket.
        // What we really want is to render the particle with the frontmost
        // z value in this depth bucket, and scaling the render distance is
        // just hack to accomplish this.  There are cases where it will fail
        // and a more robust method should be implemented.

        float size = discSize * pixelSize * 1.6f * renderZ / corrFac;
        float posScale = abs(renderZ / (position * conjugate(orientation).toMatrix3()).z);

        Point3f center(position.x * posScale,
                       position.y * posScale,
                       position.z * posScale);
        Mat3f m = orientation.toMatrix3();
        Vec3f v0 = Vec3f(-1, -1, 0) * m;
        Vec3f v1 = Vec3f( 1, -1, 0) * m;
        Vec3f v2 = Vec3f( 1,  1, 0) * m;
        Vec3f v3 = Vec3f(-1,  1, 0) * m;

        starTex->bind();
        glColor(color, a);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex(center + (v0 * size));
        glTexCoord2f(1, 0);
        glVertex(center + (v1 * size));
        glTexCoord2f(1, 1);
        glVertex(center + (v2 * size));
        glTexCoord2f(0, 1);
        glVertex(center + (v3 * size));
        glEnd();

        // If the object is brighter than magnitude 1, add a halo around it to
        // make it appear more brilliant.  This is a hack to compensate for the
        // limited dynamic range of monitors.
        //
        // TODO: Currently, this is extremely broken.  Stars look fine,
        // but planets look ridiculous with bright haloes.
        if (useHaloes && appMag < saturationMag)
        {
            a = 0.4f * clamp((appMag - saturationMag) * -0.8f);
            float s = renderZ * 0.001f * (3 - (appMag - saturationMag)) * 2;
            if (s > size * 3)
	        size = s * 2.0f/(1.0f +FOV/fov);
            else
                size = size * 3;
            float realSize = discSizeInPixels * pixelSize * renderZ;
            if (size < realSize * 10)
                size = realSize * 10;
            glareTex->bind();
            glColor(color, a);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0);
            glVertex(center + (v0 * size));
            glTexCoord2f(1, 0);
            glVertex(center + (v1 * size));
            glTexCoord2f(1, 1);
            glVertex(center + (v2 * size));
            glTexCoord2f(0, 1);
            glVertex(center + (v3 * size));
            glEnd();
        }
    }
}


static void renderBumpMappedMesh(const GLContext& context,
                                 Texture& baseTexture,
                                 Texture& bumpTexture,
                                 Vec3f lightDirection,
                                 Quatf orientation,
                                 Color ambientColor,
                                 const Frustum& frustum,
                                 float lod)
{
    // We're doing our own per-pixel lighting, so disable GL's lighting
    glDisable(GL_LIGHTING);

    // Render the base texture on the first pass . . .  The color
    // should have already been set up by the caller.
    lodSphere->render(context,
                      LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                      frustum, lod,
                      &baseTexture);

    // The 'default' light vector for the bump map is (0, 0, 1).  Determine
    // a rotation transformation that will move the sun direction to
    // this vector.
    Quatf lightOrientation;
    {
        Vec3f zeroLightDirection(0, 0, 1);
        Vec3f axis = lightDirection ^ zeroLightDirection;
        float cosAngle = zeroLightDirection * lightDirection;
        float angle = 0.0f;
        float epsilon = 1e-5f;

        if (cosAngle + 1 < epsilon)
        {
            axis = Vec3f(0, 1, 0);
            angle = (float) PI;
        }
        else if (cosAngle - 1 > -epsilon)
        {
            axis = Vec3f(0, 1, 0);
            angle = 0.0f;
        }
        else
        {
            axis.normalize();
            angle = (float) acos(cosAngle);
        }
        lightOrientation.setAxisAngle(axis, angle);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);

    // Set up the bump map with one directional light source
    SetupCombinersBumpMap(bumpTexture, *normalizationTex, ambientColor);

    // The second set texture coordinates will contain the light
    // direction in tangent space.  We'll generate the texture coordinates
    // from the surface normals using GL_NORMAL_MAP_EXT and then
    // use the texture matrix to rotate them into tangent space.
    // This method of generating tangent space light direction vectors
    // isn't as general as transforming the light direction by an
    // orthonormal basis for each mesh vertex, but it works well enough
    // for spheres illuminated by directional light sources.
    glx::glActiveTextureARB(GL_TEXTURE1_ARB);

    // Set up GL_NORMAL_MAP_EXT texture coordinate generation.  This
    // mode is part of the cube map extension.
    glEnable(GL_TEXTURE_GEN_R);
    glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_EXT);
    glEnable(GL_TEXTURE_GEN_S);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_EXT);
    glEnable(GL_TEXTURE_GEN_T);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_EXT);

    // Set up the texture transformation--the light direction and the
    // viewer orientation both need to be considered.
    glMatrixMode(GL_TEXTURE);
    glScalef(-1.0f, 1.0f, 1.0f);
    glRotate(lightOrientation * ~orientation);
    glMatrixMode(GL_MODELVIEW);
    glx::glActiveTextureARB(GL_TEXTURE0_ARB);

    lodSphere->render(context,
                      LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                      frustum, lod,
                      &bumpTexture);

    // Reset the second texture unit
    glx::glActiveTextureARB(GL_TEXTURE1_ARB);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_TEXTURE_GEN_R);
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);

    DisableCombiners();
    glDisable(GL_BLEND);
}


static void renderSmoothMesh(const GLContext& context,
                             Texture& baseTexture,
                             Vec3f lightDirection,
                             Quatf orientation,
                             Color ambientColor,
                             float lod,
                             const Frustum& frustum,
                             bool invert = false)
{
    Texture* textures[4];

    // We're doing our own per-pixel lighting, so disable GL's lighting
    glDisable(GL_LIGHTING);

    // The 'default' light vector for the bump map is (0, 0, 1).  Determine
    // a rotation transformation that will move the sun direction to
    // this vector.
    Quatf lightOrientation;
    {
        Vec3f zeroLightDirection(0, 0, 1);
        Vec3f axis = lightDirection ^ zeroLightDirection;
        float cosAngle = zeroLightDirection * lightDirection;
        float angle = 0.0f;
        float epsilon = 1e-5f;

        if (cosAngle + 1 < epsilon)
        {
            axis = Vec3f(0, 1, 0);
            angle = (float) PI;
        }
        else if (cosAngle - 1 > -epsilon)
        {
            axis = Vec3f(0, 1, 0);
            angle = 0.0f;
        }
        else
        {
            axis.normalize();
            angle = (float) acos(cosAngle);
        }
        lightOrientation.setAxisAngle(axis, angle);
    }

    SetupCombinersSmooth(baseTexture, *normalizationTex, ambientColor, invert);

    // The second set texture coordinates will contain the light
    // direction in tangent space.  We'll generate the texture coordinates
    // from the surface normals using GL_NORMAL_MAP_EXT and then
    // use the texture matrix to rotate them into tangent space.
    // This method of generating tangent space light direction vectors
    // isn't as general as transforming the light direction by an
    // orthonormal basis for each mesh vertex, but it works well enough
    // for spheres illuminated by directional light sources.
    glx::glActiveTextureARB(GL_TEXTURE1_ARB);

    // Set up GL_NORMAL_MAP_EXT texture coordinate generation.  This
    // mode is part of the cube map extension.
    glEnable(GL_TEXTURE_GEN_R);
    glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_EXT);
    glEnable(GL_TEXTURE_GEN_S);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_EXT);
    glEnable(GL_TEXTURE_GEN_T);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP_EXT);

    // Set up the texture transformation--the light direction and the
    // viewer orientation both need to be considered.
    glMatrixMode(GL_TEXTURE);
    glRotate(lightOrientation * ~orientation);
    glMatrixMode(GL_MODELVIEW);
    glx::glActiveTextureARB(GL_TEXTURE0_ARB);

    textures[0] = &baseTexture;
    lodSphere->render(context,
                      LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                      frustum, lod,
                      textures, 1);

    // Reset the second texture unit
    glx::glActiveTextureARB(GL_TEXTURE1_ARB);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_TEXTURE_GEN_R);
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);

    DisableCombiners();
}


struct RenderInfo
{
    Color color;
    Texture* baseTex;
    Texture* bumpTex;
    Texture* nightTex;
    Texture* glossTex;
    Texture* overlayTex;
    Color hazeColor;
    Color specularColor;
    float specularPower;
    Vec3f sunDir_eye;
    Vec3f sunDir_obj;
    Vec3f eyeDir_obj;
    Point3f eyePos_obj;
    Color sunColor;
    Color ambientColor;
    Quatf orientation;
    float pixWidth;
    bool useTexEnvCombine;

    RenderInfo() : color(1.0f, 1.0f, 1.0f),
                   baseTex(NULL),
                   bumpTex(NULL),
                   nightTex(NULL),
                   glossTex(NULL),
                   overlayTex(NULL),
                   hazeColor(0.0f, 0.0f, 0.0f),
                   specularColor(0.0f, 0.0f, 0.0f),
                   specularPower(0.0f),
                   sunDir_eye(0.0f, 0.0f, 1.0f),
                   sunDir_obj(0.0f, 0.0f, 1.0f),
                   eyeDir_obj(0.0f, 0.0f, 1.0f),
                   eyePos_obj(0.0f, 0.0f, 0.0f),
                   sunColor(1.0f, 1.0f, 1.0f),
                   ambientColor(0.0f, 0.0f, 0.0f),
                   orientation(1.0f, 0.0f, 0.0f, 0.0f),
                   pixWidth(1.0f),
                   useTexEnvCombine(false)
    {};
};


void renderAtmosphere(const Atmosphere& atmosphere,
                      Point3f center,
                      float radius,
                      const Vec3f& sunDirection,
                      Color ambientColor,
                      float fade,
                      bool lit)
{
    if (atmosphere.height == 0.0f)
        return;

    glDepthMask(GL_FALSE);

    Vec3f eyeVec = center - Point3f(0.0f, 0.0f, 0.0f);
    double centerDist = eyeVec.length();
    // double surfaceDist = (double) centerDist - (double) radius;

    Vec3f normal = eyeVec;
    normal = normal / (float) centerDist;

    float tangentLength = (float) sqrt(square(centerDist) - square(radius));
    float atmRadius = tangentLength * radius / (float) centerDist;
    float atmOffsetFromCenter = square(radius) / (float) centerDist;
    Point3f atmCenter = center - atmOffsetFromCenter * normal;

    Vec3f uAxis, vAxis;
    if (abs(normal.x) < abs(normal.y) && abs(normal.x) < abs(normal.z))
    {
        uAxis = Vec3f(1, 0, 0) ^ normal;
        uAxis.normalize();
    }
    else if (abs(eyeVec.y) < abs(normal.z))
    {
        uAxis = Vec3f(0, 1, 0) ^ normal;
        uAxis.normalize();
    }
    else
    {
        uAxis = Vec3f(0, 0, 1) ^ normal;
        uAxis.normalize();
    }
    vAxis = uAxis ^ normal;

    float height = atmosphere.height / radius;

    glBegin(GL_QUAD_STRIP);
    int divisions = 180;
    for (int i = 0; i <= divisions; i++)
    {
        float theta = (float) i / (float) divisions * 2 * (float) PI;
        Vec3f v = (float) cos(theta) * uAxis + (float) sin(theta) * vAxis;
        Point3f base = atmCenter + v * atmRadius;
        Vec3f toCenter = base - center;

        float cosSunAngle = (toCenter * sunDirection) / radius;
        float brightness = 1.0f;
        float botColor[3];
        float topColor[3];
        botColor[0] = atmosphere.lowerColor.red();
        botColor[1] = atmosphere.lowerColor.green();
        botColor[2] = atmosphere.lowerColor.blue();
        topColor[0] = atmosphere.upperColor.red();
        topColor[1] = atmosphere.upperColor.green();
        topColor[2] = atmosphere.upperColor.blue();

        if (cosSunAngle < 0.2f && lit)
        {
            if (cosSunAngle < -0.2f)
            {
                brightness = 0;
            }
            else
            {
                float t = (0.2f + cosSunAngle) * 2.5f;
                brightness = t;
                botColor[0] = Mathf::lerp(t, 1.0f, botColor[0]);
                botColor[1] = Mathf::lerp(t, 0.3f, botColor[1]);
                botColor[2] = Mathf::lerp(t, 0.0f, botColor[2]);
                topColor[0] = Mathf::lerp(t, 1.0f, topColor[0]);
                topColor[1] = Mathf::lerp(t, 0.3f, topColor[1]);
                topColor[2] = Mathf::lerp(t, 0.0f, topColor[2]);
            }
        }

        glColor4f(botColor[0], botColor[1], botColor[2],
                  0.85f * fade * brightness + ambientColor.red());
        glVertex(base - toCenter * height * 0.05f);
        glColor4f(topColor[0], topColor[1], topColor[2], 0.0f);
        glVertex(base + toCenter * height);
    }
    glEnd();
}


static Vec3f ellipsoidTangent(const Vec3f& recipSemiAxes,
                              const Vec3f& w,
                              const Vec3f& e,
                              const Vec3f& e_,
                              float ee)
{
    // We want to find t such that -E(1-t) + Wt is the direction of a ray
    // tangent to the ellipsoid.  A tangent ray will intersect the ellipsoid
    // at exactly one point.  Finding the intersection between a ray and an
    // ellipsoid ultimately requires using the quadratic formula, which has
    // one solution when the discriminant (b^2 - 4ac) is zero.  The code below
    // computes the value of t that results in a discriminant of zero.
    Vec3f w_(w.x * recipSemiAxes.x, w.y * recipSemiAxes.y, w.z * recipSemiAxes.z);
    float ww = w_ * w_;
    float ew = w_ * e_;

    // Before elimination of terms:
    // float a =  4 * square(ee + ew) - 4 * (ee + 2 * ew + ww) * (ee - 1.0f);
    // float b = -8 * ee * (ee + ew)  - 4 * (-2 * (ee + ew) * (ee - 1.0f));
    // float c =  4 * ee * ee         - 4 * (ee * (ee - 1.0f));

    float a =  4 * square(ee + ew) - 4 * (ee + 2 * ew + ww) * (ee - 1.0f);
    float b = -8 * (ee + ew);
    float c =  4 * ee;

    float t = 0.0f;
    float discriminant = b * b - 4 * a * c;

    if (discriminant < 0.0f)
        t = (-b + (float) sqrt(-discriminant)) / (2 * a); // Bad!
    else
        t = (-b + (float) sqrt(discriminant)) / (2 * a);

    // V is the direction vector.  We now need the point of intersection,
    // which we obtain by solving the quadratic equation for the ray-ellipse
    // intersection.  Since we already know that the discriminant is zero,
    // the solution is just -b/2a
    Vec3f v = -e * (1 - t) + w * t;
    Vec3f v_(v.x * recipSemiAxes.x, v.y * recipSemiAxes.y, v.z * recipSemiAxes.z);
    float a1 = v_ * v_;
    float b1 = 2.0f * v_ * e_;
    float t1 = -b1 / (2 * a1);

    return e + v * t1;
}


void Renderer::renderEllipsoidAtmosphere(const Atmosphere& atmosphere,
                                         Point3f center,
                                         const Quatf& orientation,
                                         Vec3f semiAxes,
                                         const Vec3f& sunDirection,
                                         Color ambientColor,
                                         float pixSize,
                                         bool lit)
{
    if (atmosphere.height == 0.0f)
        return;

    glDepthMask(GL_FALSE);

    // Gradually fade in the atmosphere if it's thickness on screen is just
    // over one pixel.
    float fade = clamp(pixSize - 2);

    Mat3f rot = orientation.toMatrix3();
    Mat3f irot = conjugate(orientation).toMatrix3();

    Point3f eyePos(0.0f, 0.0f, 0.0f);
    float radius = max(semiAxes.x, max(semiAxes.y, semiAxes.z));
    Vec3f eyeVec = center - eyePos;
    eyeVec = eyeVec * irot;
    double centerDist = eyeVec.length();

    float height = atmosphere.height / radius;
    Vec3f recipSemiAxes(1.0f / semiAxes.x, 1.0f / semiAxes.y, 1.0f / semiAxes.z);

    Vec3f recipAtmSemiAxes = recipSemiAxes / (1.0f + height);
    Mat3f A = Mat3f::scaling(recipAtmSemiAxes);
    Mat3f A1 = Mat3f::scaling(recipSemiAxes);

    // ellipDist is not the true distance from the surface unless the
    // planet is spherical.  Computing the true distance requires finding
    // the roots of a sixth degree polynomial, and isn't actually what we
    // want anyhow since the atmosphere region is just the planet ellipsoid
    // multiplied by a uniform scale factor.  The value that we do compute
    // is the distance to the surface along a line from the eye position to
    // the center of the ellipsoid.
    float ellipDist = (float) sqrt((eyeVec * A1) * (eyeVec * A1)) - 1.0f;
    bool within = ellipDist < height;

    // Adjust the tesselation of the sky dome/ring based on distance from the
    // planet surface.
    int nSlices = MaxSkySlices;
    if (ellipDist < 0.25f)
    {
        nSlices = MinSkySlices + max(0, (int) ((ellipDist / 0.25f) * (MaxSkySlices - MinSkySlices)));
        nSlices &= ~1;
    }

    int nRings = min(1 + (int) pixSize / 5, 6);
    int nHorizonRings = nRings;
    if (within)
        nRings += 12;

    float horizonHeight = height;
    if (within)
    {
        if (ellipDist <= 0.0f)
            horizonHeight = 0.0f;
        else
            horizonHeight *= max((float) pow(ellipDist / height, 0.33f), 0.001f);
    }

    Vec3f e = -eyeVec;
    Vec3f e_(e.x * recipSemiAxes.x, e.y * recipSemiAxes.y, e.z * recipSemiAxes.z);
    float ee = e_ * e_;

    // Compute the cosine of the altitude of the sun.  This is used to compute
    // the degree of sunset/sunrise coloration.
    float cosSunAltitude = 0.0f;
    {
        // Check for a sun either directly behind or in front of the viewer
        float cosSunAngle = (float) ((sunDirection * e) / centerDist);
        if (cosSunAngle < -1.0f + 1.0e-6f)
        {
            cosSunAltitude = 0.0f;
        }
        else if (cosSunAngle > 1.0f - 1.0e-6f)
        {
            cosSunAltitude = 0.0f;
        }
        else
        {
            Point3f tangentPoint = center +
                ellipsoidTangent(recipSemiAxes,
                                 (-sunDirection * irot) * (float) centerDist,
                                 e, e_, ee) * rot;
            Vec3f tangentDir = tangentPoint - eyePos;
            tangentDir.normalize();
            cosSunAltitude = sunDirection * tangentDir;
        }
    }

    Vec3f normal = eyeVec;
    normal = normal / (float) centerDist;

    Vec3f uAxis, vAxis;
    if (abs(normal.x) < abs(normal.y) && abs(normal.x) < abs(normal.z))
    {
        uAxis = Vec3f(1, 0, 0) ^ normal;
        uAxis.normalize();
    }
    else if (abs(eyeVec.y) < abs(normal.z))
    {
        uAxis = Vec3f(0, 1, 0) ^ normal;
        uAxis.normalize();
    }
    else
    {
        uAxis = Vec3f(0, 0, 1) ^ normal;
        uAxis.normalize();
    }
    vAxis = uAxis ^ normal;

    // Compute the contour of the ellipsoid
    int i;
    for (i = 0; i <= nSlices; i++)
    {
        // We want rays with an origin at the eye point and tangent to the the
        // ellipsoid.
        float theta = (float) i / (float) nSlices * 2 * (float) PI;
        Vec3f w = (float) cos(theta) * uAxis + (float) sin(theta) * vAxis;
        w = w * (float) centerDist;

        Vec3f toCenter = ellipsoidTangent(recipSemiAxes, w, e, e_, ee);
        skyContour[i].v = toCenter * rot;
        skyContour[i].centerDist = skyContour[i].v.length();
        skyContour[i].eyeDir = skyContour[i].v + (center - eyePos);
        skyContour[i].eyeDist = skyContour[i].eyeDir.length();
        skyContour[i].eyeDir.normalize();
        
        float skyCapDist = (float) sqrt(square(skyContour[i].eyeDist) +
                                        square(horizonHeight * radius));
        skyContour[i].cosSkyCapAltitude = skyContour[i].eyeDist /
            skyCapDist;
    }


    Vec3f botColor(atmosphere.lowerColor.red(),
                   atmosphere.lowerColor.green(),
                   atmosphere.lowerColor.blue());
    Vec3f topColor(atmosphere.upperColor.red(),
                   atmosphere.upperColor.green(),
                   atmosphere.upperColor.blue());
    Vec3f sunsetColor(atmosphere.sunsetColor.red(),
                      atmosphere.sunsetColor.green(),
                      atmosphere.sunsetColor.blue());
    if (within)
    {
        Vec3f skyColor(atmosphere.skyColor.red(),
                       atmosphere.skyColor.green(),
                       atmosphere.skyColor.blue());
        if (ellipDist < 0.0f)
            topColor = skyColor;
        else
            topColor = skyColor + (topColor - skyColor) * (ellipDist / height);
    }

    Vec3f zenith = (skyContour[0].v + skyContour[nSlices / 2].v);
    zenith.normalize();
    zenith *= skyContour[0].centerDist * (1.0f + horizonHeight * 2.0f);

    float minOpacity = within ? (1.0f - ellipDist / height) * 0.75f : 0.0f;
    float sunset = cosSunAltitude < 0.9f ? 0.0f : (cosSunAltitude - 0.9f) * 10.0f;

    // Build the list of vertices
    SkyVertex* vtx = skyVertices;
    for (i = 0; i <= nRings; i++)
    {
        float h = min(1.0f, (float) i / (float) nHorizonRings);
        float hh = (float) sqrt(h);
        float u = i <= nHorizonRings ? 0.0f :
            (float) (i - nHorizonRings) / (float) (nRings - nHorizonRings);
        float r = Mathf::lerp(h, 1.0f - (horizonHeight * 0.05f), 1.0f + horizonHeight);
        float atten = 1.0f - hh;

        for (int j = 0; j < nSlices; j++)
        {
            Vec3f v;
            if (i <= nHorizonRings)
                v = skyContour[j].v * r;
            else
                v = (skyContour[j].v * (1.0f - u) + zenith * u) * r;
            Point3f p = center + v;


            Vec3f viewDir(p.x, p.y, p.z);
            viewDir.normalize();
            float cosSunAngle = viewDir * sunDirection;
            float cosAltitude = viewDir * skyContour[j].eyeDir;
            float brightness = 1.0f;
            float coloration = 0.0f;
            if (lit)
            {
                if (sunset > 0.0f && cosSunAngle > 0.7f && cosAltitude > 0.98f)
                {
                    coloration =  (1.0f / 0.30f) * (cosSunAngle - 0.70f);
                    coloration *= 50.0f * (cosAltitude - 0.98f);
                    coloration *= sunset;
                }   
            
                cosSunAngle = (skyContour[j].v * sunDirection) / skyContour[j].centerDist;
                if (cosSunAngle > -0.2f)
                {
                    if (cosSunAngle < 0.3f)
                        brightness = (cosSunAngle + 0.2f) * 2.0f;
                    else
                        brightness = 1.0f;
                }
                else
                {
                    brightness = 0.0f;
                }
            }

            vtx->x = p.x;
            vtx->y = p.y;
            vtx->z = p.z;

#if 0
            // Better way of generating sky color gradients--based on 
            // altitude angle.
            if (!within)
            {
                hh = (1.0f - cosAltitude) / (1.0f - skyContour[j].cosSkyCapAltitude);
            }
            else
            {
                float top = pow((ellipDist / height), 0.125f) * skyContour[j].cosSkyCapAltitude;
                if (cosAltitude < top)
                    hh = 1.0f;
                else
                    hh = (1.0f - cosAltitude) / (1.0f - top);
            }
            hh = sqrt(hh);
            //hh = (float) pow(hh, 0.25f);
#endif

            atten = 1.0f - hh;
            Vec3f color = (1.0f - hh) * botColor + hh * topColor;
            brightness *= minOpacity + (1.0f - minOpacity) * fade * atten;
            if (coloration != 0.0f)
                color = (1.0f - coloration) * color + coloration * sunsetColor;

            Color(brightness * color.x,
                  brightness * color.y,
                  brightness * color.z,
                  fade * (minOpacity + (1.0f - minOpacity)) * atten).get(vtx->color);
            vtx++;
        }
    }

    // Create the index list
    int index = 0;
    for (i = 0; i < nRings; i++)
    {
        int baseVertex = i * nSlices;
        for (int j = 0; j < nSlices; j++)
        {
            skyIndices[index++] = baseVertex + j;
            skyIndices[index++] = baseVertex + nSlices + j;
        }
        skyIndices[index++] = baseVertex;
        skyIndices[index++] = baseVertex + nSlices;
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(SkyVertex), &skyVertices[0].x);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SkyVertex),
                   static_cast<void*>(&skyVertices[0].color));

    for (i = 0; i < nRings; i++)
    {
        glDrawElements(GL_QUAD_STRIP,
                       (nSlices + 1) * 2,
                       GL_UNSIGNED_INT,
                       &skyIndices[(nSlices + 1) * 2 * i]);
    }

    glDisableClientState(GL_COLOR_ARRAY);
}


static void setupNightTextureCombine()
{
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PRIMARY_COLOR_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_ONE_MINUS_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
}


static void setupBumpTexenv()
{
    // Set up the texenv_combine extension to do DOT3 bump mapping.
    // No support for ambient light yet.
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);

    // The primary color contains the light direction in surface
    // space, and texture0 is a normal map.  The lighting is
    // calculated by computing the dot product.
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_DOT3_RGB_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PRIMARY_COLOR_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);

    // In the final stage, modulate the lighting value by the
    // base texture color.
    glx::glActiveTextureARB(GL_TEXTURE1_ARB);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PREVIOUS_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
    glEnable(GL_TEXTURE_2D);

    glx::glActiveTextureARB(GL_TEXTURE0_ARB);
}


static void setupBumpTexenvAmbient(Color ambientColor)
{
    float texenvConst[4];
    texenvConst[0] = ambientColor.red();
    texenvConst[1] = ambientColor.green();
    texenvConst[2] = ambientColor.blue();
    texenvConst[3] = ambientColor.alpha();

    // Set up the texenv_combine extension to do DOT3 bump mapping.
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);

    // The primary color contains the light direction in surface
    // space, and texture0 is a normal map.  The lighting is
    // calculated by computing the dot product.
    glx::glActiveTextureARB(GL_TEXTURE0_ARB);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_DOT3_RGB_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PRIMARY_COLOR_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);

    // Add in the ambient color
    glx::glActiveTextureARB(GL_TEXTURE1_ARB);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, texenvConst);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_ADD);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PREVIOUS_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_CONSTANT_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
    glEnable(GL_TEXTURE_2D);

    // In the final stage, modulate the lighting value by the
    // base texture color.
    glx::glActiveTextureARB(GL_TEXTURE2_ARB);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PREVIOUS_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
    glEnable(GL_TEXTURE_2D);

    glx::glActiveTextureARB(GL_TEXTURE0_ARB);
}


static void setupTexenvAmbient(Color ambientColor)
{
    float texenvConst[4];
    texenvConst[0] = ambientColor.red();
    texenvConst[1] = ambientColor.green();
    texenvConst[2] = ambientColor.blue();
    texenvConst[3] = ambientColor.alpha();

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);

    // The primary color contains the light direction in surface
    // space, and texture0 is a normal map.  The lighting is
    // calculated by computing the dot product.
    glx::glActiveTextureARB(GL_TEXTURE0_ARB);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, texenvConst);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_CONSTANT_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
    glEnable(GL_TEXTURE_2D);
}


static void setupTexenvGlossMapAlpha()
{
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PRIMARY_COLOR_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_ALPHA);

}


static void renderModelDefault(Model* model,
                               const RenderInfo& ri,
                               bool lit)
{
    FixedFunctionRenderContext rc;
    //rc.makeCurrent();

    if (lit)
        glEnable(GL_LIGHTING);
    else
        glDisable(GL_LIGHTING);

    if (ri.baseTex == NULL)
    {
        glDisable(GL_TEXTURE_2D);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
        ri.baseTex->bind();
    }

    glColor(ri.color);

    if (ri.baseTex != NULL)
        rc.lock();

    model->render(rc);

    // Reset the material
    float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float zero = 0.0f;
    glColor4fv(black);
    glMaterialfv(GL_FRONT, GL_EMISSION, black);
    glMaterialfv(GL_FRONT, GL_SPECULAR, black);
    glMaterialfv(GL_FRONT, GL_SHININESS, &zero);
}


static void renderSphereDefault(const RenderInfo& ri,
                                const Frustum& frustum,
                                bool lit,
                                const GLContext& context)
{
    if (lit)
        glEnable(GL_LIGHTING);
    else
        glDisable(GL_LIGHTING);

    if (ri.baseTex == NULL)
    {
        glDisable(GL_TEXTURE_2D);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
        ri.baseTex->bind();
    }

    glColor(ri.color);

    lodSphere->render(context,
                      LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                      frustum, ri.pixWidth,
                      ri.baseTex);
    if (ri.nightTex != NULL && ri.useTexEnvCombine)
    {
        ri.nightTex->bind();
        setupNightTextureCombine();
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glAmbientLightColor(Color::Black); // Disable ambient light
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.nightTex);
        glAmbientLightColor(ri.ambientColor);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    if (ri.overlayTex != NULL)
    {
        ri.overlayTex->bind();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.overlayTex);
        glBlendFunc(GL_ONE, GL_ONE);
    }
}


static void renderSphere_Combiners(const RenderInfo& ri,
                                   const Frustum& frustum,
                                   const GLContext& context)
{
    glDisable(GL_LIGHTING);

    if (ri.baseTex == NULL)
    {
        glDisable(GL_TEXTURE_2D);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
        ri.baseTex->bind();
    }

    glColor(ri.color * ri.sunColor);

    if (ri.bumpTex != NULL)
    {
        renderBumpMappedMesh(context,
                             *(ri.baseTex),
                             *(ri.bumpTex),
                             ri.sunDir_eye,
                             ri.orientation,
                             ri.ambientColor,
                             frustum,
                             ri.pixWidth);
    }
    else if (ri.baseTex != NULL)
    {
        renderSmoothMesh(context,
                         *(ri.baseTex),
                         ri.sunDir_eye,
                         ri.orientation,
                         ri.ambientColor,
                         ri.pixWidth,
                         frustum);
    }
    else
    {
        glEnable(GL_LIGHTING);
        lodSphere->render(context, frustum, ri.pixWidth, NULL, 0);
    }

    if (ri.nightTex != NULL)
    {
        ri.nightTex->bind();
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        renderSmoothMesh(context,
                         *(ri.nightTex),
                         ri.sunDir_eye, 
                         ri.orientation,
                         Color::Black,
                         ri.pixWidth,
                         frustum,
                         true);
    }

    if (ri.overlayTex != NULL)
    {
        glEnable(GL_LIGHTING);
        ri.overlayTex->bind();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0, 
                          frustum, ri.pixWidth,
                          ri.overlayTex);
#if 0
        renderSmoothMesh(context,
                         *(ri.overlayTex),
                         ri.sunDir_eye,
                         ri.orientation,
                         ri.ambientColor,
                         ri.pixWidth,
                         frustum);
#endif
        glBlendFunc(GL_ONE, GL_ONE);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
}


static void renderSphere_DOT3_VP(const RenderInfo& ri,
                                 const Frustum& frustum,
                                 const GLContext& context)
{
    VertexProcessor* vproc = context.getVertexProcessor();
    assert(vproc != NULL);

    if (ri.baseTex == NULL)
    {
        glDisable(GL_TEXTURE_2D);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
        ri.baseTex->bind();
    }

    vproc->enable();
    vproc->parameter(vp::EyePosition, ri.eyePos_obj);
    vproc->parameter(vp::SunDirection, ri.sunDir_obj);
    vproc->parameter(vp::DiffuseColor, ri.sunColor * ri.color);
    vproc->parameter(vp::SpecularExponent, 0.0f, 1.0f, 0.5f, ri.specularPower);
    vproc->parameter(vp::SpecularColor, ri.sunColor * ri.specularColor);
    vproc->parameter(vp::AmbientColor, ri.ambientColor * ri.color);

    if (ri.bumpTex != NULL && ri.baseTex != NULL)
    {
        // We don't yet handle the case where there's a bump map but no
        // base texture.
        vproc->use(vp::diffuseBump);
        if (ri.ambientColor != Color::Black)
        {
            // If there's ambient light, we'll need to render in two passes:
            // one for the ambient light, and the second for light from the star.
            // We could do this in a single pass using three texture stages, but
            // this isn't won't work with hardware that only supported two
            // texture stages.

            // Render the base texture modulated by the ambient color
            setupTexenvAmbient(ri.ambientColor);
            lodSphere->render(context,
                              LODSphereMesh::TexCoords0 | LODSphereMesh::VertexProgParams,
                              frustum, ri.pixWidth,
                              ri.baseTex);

            // Add the light from the sun
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            setupBumpTexenv();
            lodSphere->render(context,
                              LODSphereMesh::Normals | LODSphereMesh::Tangents |
                              LODSphereMesh::TexCoords0 | LODSphereMesh::VertexProgParams,
                              frustum, ri.pixWidth,
                              ri.bumpTex, ri.baseTex);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glDisable(GL_BLEND);
        }
        else
        {
            glx::glActiveTextureARB(GL_TEXTURE1_ARB);
            ri.baseTex->bind();
            glx::glActiveTextureARB(GL_TEXTURE0_ARB);
            ri.bumpTex->bind();
            setupBumpTexenv();
            lodSphere->render(context,
                              LODSphereMesh::Normals | LODSphereMesh::Tangents |
                              LODSphereMesh::TexCoords0 | LODSphereMesh::VertexProgParams,
                              frustum, ri.pixWidth,
                              ri.bumpTex, ri.baseTex);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        }
    }
    else
    {
        vproc->use(vp::diffuse);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0 |
                          LODSphereMesh::VertexProgParams,
                          frustum, ri.pixWidth,
                          ri.baseTex);
    }

    // Render a specular pass; can't be done in one pass because
    // specular needs to be modulated with a gloss map.
    if (ri.specularColor != Color::Black)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        vproc->use(vp::glossMap);

        if (ri.glossTex != NULL)
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        else
            setupTexenvGlossMapAlpha();

        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.glossTex != NULL ? ri.glossTex : ri.baseTex);

        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glDisable(GL_BLEND);
    }

    if (ri.nightTex != NULL)
    {
        ri.nightTex->bind();
        vproc->use(vp::nightLights);
        setupNightTextureCombine();
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.nightTex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    if (ri.overlayTex != NULL)
    {
        ri.overlayTex->bind();
        vproc->use(vp::diffuse);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.overlayTex);
        glBlendFunc(GL_ONE, GL_ONE);
    }

    vproc->disable();
}


static void renderSphere_Combiners_VP(const RenderInfo& ri,
                                      const Frustum& frustum,
                                      const GLContext& context)
{
    Texture* textures[4];
    VertexProcessor* vproc = context.getVertexProcessor();
    assert(vproc != NULL);

    if (ri.baseTex == NULL)
    {
        glDisable(GL_TEXTURE_2D);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
        ri.baseTex->bind();
    }

    // Compute the half angle vector required for specular lighting
    Vec3f halfAngle_obj = ri.eyeDir_obj + ri.sunDir_obj;
    if (halfAngle_obj.length() != 0.0f)
        halfAngle_obj.normalize();

    // Set up the fog parameters if the haze density is non-zero
    float hazeDensity = ri.hazeColor.alpha();

    if (hazeDensity > 0.0f && !buggyVertexProgramEmulation)
    {
        glEnable(GL_FOG);
        float fogColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        fogColor[0] = ri.hazeColor.red();
        fogColor[1] = ri.hazeColor.green();
        fogColor[2] = ri.hazeColor.blue();
        glFogfv(GL_FOG_COLOR, fogColor);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 0.0);
        glFogf(GL_FOG_END, 1.0f / hazeDensity);
    }

    vproc->enable();

    vproc->parameter(vp::EyePosition, ri.eyePos_obj);
    vproc->parameter(vp::SunDirection, ri.sunDir_obj);
    vproc->parameter(vp::DiffuseColor, ri.sunColor * ri.color);
    vproc->parameter(vp::SpecularExponent, 0.0f, 1.0f, 0.5f, ri.specularPower);
    vproc->parameter(vp::SpecularColor, ri.sunColor * ri.specularColor);
    vproc->parameter(vp::AmbientColor, ri.ambientColor * ri.color);
    vproc->parameter(vp::HazeColor, ri.hazeColor);

    if (ri.bumpTex != NULL)
    {
        if (hazeDensity > 0.0f)
            vproc->use(vp::diffuseBumpHaze);
        else
            vproc->use(vp::diffuseBump);
        SetupCombinersDecalAndBumpMap(*(ri.bumpTex),
                                      ri.ambientColor * ri.color,
                                      ri.sunColor * ri.color);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::Tangents |
                          LODSphereMesh::TexCoords0 | LODSphereMesh::VertexProgParams,
                          frustum, ri.pixWidth,
                          ri.baseTex, ri.bumpTex);
        DisableCombiners();

        // Render a specular pass
        if (ri.specularColor != Color::Black)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glEnable(GL_COLOR_SUM_EXT);
            vproc->use(vp::specular);

            // Disable ambient and diffuse
            vproc->parameter(vp::AmbientColor, Color::Black);
            vproc->parameter(vp::DiffuseColor, Color::Black);
            SetupCombinersGlossMap(ri.glossTex != NULL ? GL_TEXTURE0_ARB : 0);

            textures[0] = ri.glossTex != NULL ? ri.glossTex : ri.baseTex;
            lodSphere->render(context,
                              LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                              frustum, ri.pixWidth,
                              textures, 1);

            // re-enable diffuse
            vproc->parameter(vp::DiffuseColor, ri.sunColor * ri.color);

            DisableCombiners();
            glDisable(GL_COLOR_SUM_EXT);
            glDisable(GL_BLEND);
        }
    }
    else if (ri.specularColor != Color::Black)
    {
        glEnable(GL_COLOR_SUM_EXT);
        vproc->use(vp::specular);
        SetupCombinersGlossMapWithFog(ri.glossTex != NULL ? GL_TEXTURE1_ARB : 0);
        unsigned int attributes = LODSphereMesh::Normals | LODSphereMesh::TexCoords0 |
            LODSphereMesh::VertexProgParams;
        lodSphere->render(context,
                          attributes, frustum, ri.pixWidth,
                          ri.baseTex, ri.glossTex);
        DisableCombiners();
        glDisable(GL_COLOR_SUM_EXT);
    }
    else
    {
        if (hazeDensity > 0.0f)
            vproc->use(vp::diffuseHaze);
        else
            vproc->use(vp::diffuse);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0 |
                          LODSphereMesh::VertexProgParams,
                          frustum, ri.pixWidth,
                          ri.baseTex);
    }

    if (hazeDensity > 0.0f)
        glDisable(GL_FOG);

    if (ri.nightTex != NULL)
    {
        ri.nightTex->bind();
        vproc->use(vp::nightLights);
        setupNightTextureCombine();
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.nightTex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    if (ri.overlayTex != NULL)
    {
        ri.overlayTex->bind();
        vproc->use(vp::diffuse);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.overlayTex);
        glBlendFunc(GL_ONE, GL_ONE);
    }

    vproc->disable();
}


// Render a planet sphere using both fragment and vertex programs
static void renderSphere_FP_VP(const RenderInfo& ri,
                               const Frustum& frustum,
                               const GLContext& context)
{
    Texture* textures[4];
    VertexProcessor* vproc = context.getVertexProcessor();
    FragmentProcessor* fproc = context.getFragmentProcessor();
    assert(vproc != NULL && fproc != NULL);

    if (ri.baseTex == NULL)
    {
        glDisable(GL_TEXTURE_2D);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
        ri.baseTex->bind();
    }

    // Compute the half angle vector required for specular lighting
    Vec3f halfAngle_obj = ri.eyeDir_obj + ri.sunDir_obj;
    if (halfAngle_obj.length() != 0.0f)
        halfAngle_obj.normalize();

    // Set up the fog parameters if the haze density is non-zero
    float hazeDensity = ri.hazeColor.alpha();

    if (hazeDensity > 0.0f)
    {
        glEnable(GL_FOG);
        float fogColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        fogColor[0] = ri.hazeColor.red();
        fogColor[1] = ri.hazeColor.green();
        fogColor[2] = ri.hazeColor.blue();
        glFogfv(GL_FOG_COLOR, fogColor);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 0.0);
        glFogf(GL_FOG_END, 1.0f / hazeDensity);
    }

    vproc->enable();

    vproc->parameter(vp::EyePosition, ri.eyePos_obj);
    vproc->parameter(vp::SunDirection, ri.sunDir_obj);
    vproc->parameter(vp::DiffuseColor, ri.sunColor * ri.color);
    vproc->parameter(vp::SpecularExponent, 0.0f, 1.0f, 0.5f, ri.specularPower);
    vproc->parameter(vp::SpecularColor, ri.sunColor * ri.specularColor);
    vproc->parameter(vp::AmbientColor, ri.ambientColor * ri.color);
    vproc->parameter(vp::HazeColor, ri.hazeColor);

    if (ri.bumpTex != NULL)
    {
        fproc->enable();
        
        if (hazeDensity > 0.0f)
            vproc->use(vp::diffuseBumpHaze);
        else
            vproc->use(vp::diffuseBump);
        fproc->use(fp::texDiffuseBump);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::Tangents |
                          LODSphereMesh::TexCoords0 | LODSphereMesh::VertexProgParams,
                          frustum, ri.pixWidth,
                          ri.baseTex, ri.bumpTex);
        fproc->disable();

        // Render a specular pass
        if (ri.specularColor != Color::Black)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glEnable(GL_COLOR_SUM_EXT);
            vproc->use(vp::specular);

            // Disable ambient and diffuse
            vproc->parameter(vp::AmbientColor, Color::Black);
            vproc->parameter(vp::DiffuseColor, Color::Black);
            SetupCombinersGlossMap(ri.glossTex != NULL ? GL_TEXTURE0_ARB : 0);

            textures[0] = ri.glossTex != NULL ? ri.glossTex : ri.baseTex;
            lodSphere->render(context,
                              LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                              frustum, ri.pixWidth,
                              textures, 1);

            // re-enable diffuse
            vproc->parameter(vp::DiffuseColor, ri.sunColor * ri.color);

            DisableCombiners();
            glDisable(GL_COLOR_SUM_EXT);
            glDisable(GL_BLEND);
        }
    }
    else if (ri.specularColor != Color::Black)
    {
        vproc->use(vp::perFragmentSpecular);
        fproc->enable();
        fproc->use(fp::texSpecular);
        fproc->parameter(fp::DiffuseColor, ri.sunColor * ri.color);
        fproc->parameter(fp::SunDirection, ri.sunDir_obj);
        fproc->parameter(fp::SpecularColor, ri.specularColor);
        fproc->parameter(fp::SpecularExponent, ri.specularPower, 0.0f, 0.0f, 0.0f);
        fproc->parameter(fp::AmbientColor, ri.ambientColor);

        unsigned int attributes = LODSphereMesh::Normals |
                                  LODSphereMesh::TexCoords0 |
                                  LODSphereMesh::VertexProgParams;
        lodSphere->render(context,
                          attributes, frustum, ri.pixWidth,
                          ri.baseTex, ri.glossTex);
        fproc->disable();
    }
    else
    {
        fproc->enable();
        if (hazeDensity > 0.0f)
            vproc->use(vp::diffuseHaze);
        else
            vproc->use(vp::diffuse);
        fproc->use(fp::texDiffuse);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0 |
                          LODSphereMesh::VertexProgParams,
                          frustum, ri.pixWidth,
                          ri.baseTex);
        fproc->disable();
    }

    if (hazeDensity > 0.0f)
        glDisable(GL_FOG);

    if (ri.nightTex != NULL)
    {
        ri.nightTex->bind();
        vproc->use(vp::nightLights);
        setupNightTextureCombine();
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.nightTex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    if (ri.overlayTex != NULL)
    {
        ri.overlayTex->bind();
        vproc->use(vp::diffuse);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                          frustum, ri.pixWidth,
                          ri.overlayTex);
        glBlendFunc(GL_ONE, GL_ONE);
    }

    vproc->disable();
}


static void texGenPlane(GLenum coord, GLenum mode, const Vec4f& plane)
{
    float f[4];
    f[0] = plane.x; f[1] = plane.y; f[2] = plane.z; f[3] = plane.w;
    glTexGenfv(coord, mode, f);
}

static void renderShadowedModelDefault(Model* model,
                                       const RenderInfo& ri,
                                       const Frustum& frustum,
                                       float *sPlane,
                                       float *tPlane,
                                       const Vec3f& lightDir,
                                       bool useShadowMask,
                                       const GLContext& context)
{
    glEnable(GL_TEXTURE_GEN_S);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGenfv(GL_S, GL_OBJECT_PLANE, sPlane);
    //texGenPlane(GL_S, GL_OBJECT_PLANE, sPlane);
    glEnable(GL_TEXTURE_GEN_T);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGenfv(GL_T, GL_OBJECT_PLANE, tPlane);

    if (useShadowMask)
    {
        glx::glActiveTextureARB(GL_TEXTURE1_ARB);
        glEnable(GL_TEXTURE_GEN_S);
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        texGenPlane(GL_S, GL_OBJECT_PLANE,
                    Vec4f(lightDir.x, lightDir.y, lightDir.z, 0.5f));
        glx::glActiveTextureARB(GL_TEXTURE0_ARB);
    }

    glColor4f(1, 1, 1, 1);
    glDisable(GL_LIGHTING);

    if (model == NULL)
    {
        lodSphere->render(context,
                          LODSphereMesh::Normals | LODSphereMesh::Multipass,
                          frustum, ri.pixWidth, NULL);
    }
    else
    {
        FixedFunctionRenderContext rc;
        model->render(rc);
    }
    glEnable(GL_LIGHTING);

    if (useShadowMask)
    {
        glx::glActiveTextureARB(GL_TEXTURE1_ARB);
        glDisable(GL_TEXTURE_GEN_S);
        glx::glActiveTextureARB(GL_TEXTURE0_ARB);
    }
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
}


static void renderShadowedModelVertexShader(const RenderInfo& ri,
                                            const Frustum& frustum,
                                            float* sPlane, float* tPlane,
                                            Vec3f& lightDir,
                                            const GLContext& context)
{
    VertexProcessor* vproc = context.getVertexProcessor();
    assert(vproc != NULL);

    vproc->enable();
    vproc->parameter(vp::SunDirection, lightDir);
    vproc->parameter(vp::TexGen_S, sPlane);
    vproc->parameter(vp::TexGen_T, tPlane);
    vproc->use(vp::shadowTexture);

    lodSphere->render(context,
                      LODSphereMesh::Normals | LODSphereMesh::Multipass, frustum,
                      ri.pixWidth, NULL);

    vproc->disable();
}


static void renderRings(RingSystem& rings,
                        RenderInfo& ri,
                        float planetRadius,
                        float planetOblateness,
                        unsigned int textureResolution,
                        bool renderShadow,
                        const GLContext& context,
                        unsigned int nSections)
{
    float inner = rings.innerRadius / planetRadius;
    float outer = rings.outerRadius / planetRadius;

    // Ring Illumination:
    // Since a ring system is composed of millions of individual
    // particles, it's not at all realistic to model it as a flat
    // Lambertian surface.  We'll approximate the llumination
    // function by assuming that the ring system contains Lambertian
    // particles, and that the brightness at some point in the ring
    // system is proportional to the illuminated fraction of a
    // particle there.  In fact, we'll simplify things further and
    // set the illumination of the entire ring system to the same
    // value, computing the illuminated fraction of a hypothetical
    // particle located at the center of the planet.  This
    // approximation breaks down when you get close to the planet.
    float ringIllumination = 0.0f;
    {
        float illumFraction = (1.0f + ri.eyeDir_obj * ri.sunDir_obj) / 2.0f;
        // Just use the illuminated fraction for now . . .
        ringIllumination = illumFraction;
    }

    GLContext::VertexPath vpath = context.getVertexPath();
    VertexProcessor* vproc = context.getVertexProcessor();
    FragmentProcessor* fproc = context.getFragmentProcessor();

    if (vproc != NULL)
    {
        vproc->enable();
        vproc->use(vp::ringIllum);
        vproc->parameter(vp::SunDirection, ri.sunDir_obj);
        vproc->parameter(vp::DiffuseColor, ri.sunColor * rings.color);
        vproc->parameter(vp::AmbientColor, ri.ambientColor * ri.color);
        vproc->parameter(vp::Constant0, Vec3f(0, 0.5, 1.0));
    }

    // If we have multi-texture support, we'll use the second texture unit
    // to render the shadow of the planet on the rings.  This is a bit of
    // a hack, and assumes that the planet is ellipsoidal in shape,
    // and only works for a planet illuminated by a single sun where the
    // distance to the sun is very large relative to its diameter.
    if (renderShadow)
    {
        glx::glActiveTextureARB(GL_TEXTURE1_ARB);
        glEnable(GL_TEXTURE_2D);
        shadowTex->bind();

        float sPlane[4] = { 0, 0, 0, 0.5f };
        float tPlane[4] = { 0, 0, 0, 0.5f };

        // Compute the projection vectors based on the sun direction.
        // I'm being a little careless here--if the sun direction lies
        // along the y-axis, this will fail.  It's unlikely that a
        // planet would ever orbit underneath its sun (an orbital
        // inclination of 90 degrees), but this should be made
        // more robust anyway.
        Vec3f axis = Vec3f(0, 1, 0) ^ ri.sunDir_obj;
        float cosAngle = Vec3f(0.0f, 1.0f, 0.0f) * ri.sunDir_obj;
        float angle = (float) acos(cosAngle);
        axis.normalize();

        float sScale = 1.0f;
        float tScale = 1.0f;
        if (fproc == NULL)
        {
            // When fragment programs aren't used, we render shadows with circular
            // textures.  We scale up the texture slightly to account for the
            // padding pixels near the texture borders.
            sScale *= ShadowTextureScale;
            tScale *= ShadowTextureScale;
        }

        if (planetOblateness != 0.0f)
        {
            // For oblate planets, the size of the shadow volume will vary based
            // on the light direction.

            // A vertical slice of the planet is an ellipse
            float a = 1.0f;                          // semimajor axis
            float b = a * (1.0f - planetOblateness); // semiminor axis
            float ecc2 = 1.0f - (b * b) / (a * a);   // square of eccentricity

            // Calculate the radius of the ellipse at the incident angle of the
            // light on the ring plane + 90 degrees.
            float r = a * (float) sqrt((1.0f - ecc2) /
                                       (1.0f - ecc2 * square(cosAngle)));
            
            tScale *= a / r;
        }

        // The s axis is perpendicular to the shadow axis in the plane of the
        // of the rings, and the t axis completes the orthonormal basis.
        Vec3f sAxis = axis * 0.5f;
        Vec3f tAxis = (axis ^ ri.sunDir_obj) * 0.5f * tScale;

        sPlane[0] = sAxis.x; sPlane[1] = sAxis.y; sPlane[2] = sAxis.z;
        tPlane[0] = tAxis.x; tPlane[1] = tAxis.y; tPlane[2] = tAxis.z;

        if (vproc != NULL)
        {
            vproc->parameter(vp::TexGen_S, sPlane);
            vproc->parameter(vp::TexGen_T, tPlane);
        }
        else
        {
            glEnable(GL_TEXTURE_GEN_S);
            glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
            glTexGenfv(GL_S, GL_EYE_PLANE, sPlane);
            glEnable(GL_TEXTURE_GEN_T);
            glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
            glTexGenfv(GL_T, GL_EYE_PLANE, tPlane);
        }

        glx::glActiveTextureARB(GL_TEXTURE0_ARB);

        if (fproc != NULL)
        {
            float r0 = 0.24f;
            float r1 = 0.25f;
            float bias = 1.0f / (1.0f - r1 / r0);
            float scale = -bias / r0;

            fproc->enable();
            fproc->use(fp::sphereShadowOnRings);
            fproc->parameter(fp::ShadowParams0, scale, bias, 0.0f, 0.0f);
            fproc->parameter(fp::AmbientColor, ri.ambientColor * ri.color);
        }
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Texture* ringsTex = rings.texture.find(textureResolution);

    if (ringsTex != NULL)
        ringsTex->bind();
    else
        glDisable(GL_TEXTURE_2D);
        
    // Perform our own lighting for the rings.
    // TODO: Don't forget about light source color (required when we
    // paying attention to star color.)
    if (vpath == GLContext::VPath_Basic)
    {
        glDisable(GL_LIGHTING);
        Vec3f litColor(rings.color.red(), rings.color.green(), rings.color.blue());
        litColor = litColor * ringIllumination +
            Vec3f(ri.ambientColor.red(), ri.ambientColor.green(),
                  ri.ambientColor.blue());
        glColor4f(litColor.x, litColor.y, litColor.z, 1.0f);
    }

    // This gets tricky . . .  we render the rings in two parts.  One
    // part is potentially shadowed by the planet, and we need to
    // render that part with the projected shadow texture enabled.
    // The other part isn't shadowed, but will appear so if we don't
    // first disable the shadow texture.  The problem is that the
    // shadow texture will affect anything along the line between the
    // sun and the planet, regardless of whether it's in front or
    // behind the planet.

    // Compute the angle of the sun projected on the ring plane
    float sunAngle = (float) atan2(ri.sunDir_obj.z, ri.sunDir_obj.x);

    // If there's a fragment program, it will handle the ambient term--make
    // sure that we don't add it both in the fragment and vertex programs.
    if (vproc != NULL && fproc != NULL)
        glAmbientLightColor(Color::Black);

    renderRingSystem(inner, outer,
                     (float) (sunAngle + PI / 2),
                     (float) (sunAngle + 3 * PI / 2),
                     nSections / 2);
    renderRingSystem(inner, outer,
                     (float) (sunAngle +  3 * PI / 2),
                     (float) (sunAngle + PI / 2),
                     nSections / 2);

    if (vproc != NULL && fproc != NULL)
        glAmbientLightColor(ri.ambientColor * ri.color);

    // Disable the second texture unit if it was used
    if (renderShadow)
    {
        glx::glActiveTextureARB(GL_TEXTURE1_ARB);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
        glx::glActiveTextureARB(GL_TEXTURE0_ARB);

        if (fproc != NULL)
            fproc->disable();
    }

    // Render the unshadowed side
    renderRingSystem(inner, outer,
                     (float) (sunAngle - PI / 2),
                     (float) (sunAngle + PI / 2),
                     nSections / 2);
    renderRingSystem(inner, outer,
                     (float) (sunAngle + PI / 2),
                     (float) (sunAngle - PI / 2),
                     nSections / 2);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    if (vproc != NULL)
        vproc->disable();
}


static void
renderEclipseShadows(Model* model,
                     vector<Renderer::EclipseShadow>& eclipseShadows,
                     RenderInfo& ri,
                     float planetRadius,
                     Mat4f& planetMat,
                     Frustum& viewFrustum,
                     const GLContext& context)
{
    // Eclipse shadows on mesh objects aren't working yet.
    if (model != NULL)
        return;

    for (vector<Renderer::EclipseShadow>::iterator iter = eclipseShadows.begin();
         iter != eclipseShadows.end(); iter++)
    {
        Renderer::EclipseShadow shadow = *iter;

#ifdef DEBUG_ECLIPSE_SHADOWS
        // Eclipse debugging: render the central axis of the eclipse
        // shadow volume.
        glDisable(GL_TEXTURE_2D);
        glColor4f(1, 0, 0, 1);
        Point3f blorp = shadow.origin * planetMat;
        Vec3f blah = shadow.direction * planetMat;
        blorp.x /= planetRadius; blorp.y /= planetRadius; blorp.z /= planetRadius;
        float foo = blorp.distanceFromOrigin();
        glBegin(GL_LINES);
        glVertex(blorp);
        glVertex(blorp + foo * blah);
        glEnd();
        glEnable(GL_TEXTURE_2D);
#endif

        // Determine which eclipse shadow texture to use.  This is only
        // a very rough approximation to reality.  Since there are an
        // infinite number of possible eclipse volumes, what we should be
        // doing is generating the eclipse textures on the fly using
        // render-to-texture.  But for now, we'll just choose from a fixed
        // set of eclipse shadow textures based on the relative size of
        // the umbra and penumbra.
        Texture* eclipseTex = NULL;
        float umbra = shadow.umbraRadius / shadow.penumbraRadius;
        if (umbra < 0.1f)
            eclipseTex = eclipseShadowTextures[0];
        else if (umbra < 0.35f)
            eclipseTex = eclipseShadowTextures[1];
        else if (umbra < 0.6f)
            eclipseTex = eclipseShadowTextures[2];
        else if (umbra < 0.9f)
            eclipseTex = eclipseShadowTextures[3];
        else
            eclipseTex = shadowTex;

        // Compute the transformation to use for generating texture
        // coordinates from the object vertices.
        Point3f origin = shadow.origin * planetMat;
        Vec3f dir = shadow.direction * planetMat;
        float scale = planetRadius / shadow.penumbraRadius;
        Vec3f axis = Vec3f(0, 1, 0) ^ dir;
        float angle = (float) acos(Vec3f(0, 1, 0) * dir);
        axis.normalize();
        Mat4f mat = Mat4f::rotation(axis, -angle);
        Vec3f sAxis = Vec3f(0.5f * scale, 0, 0) * mat;
        Vec3f tAxis = Vec3f(0, 0, 0.5f * scale) * mat;

        float sPlane[4] = { 0, 0, 0, 0 };
        float tPlane[4] = { 0, 0, 0, 0 };
        sPlane[0] = sAxis.x; sPlane[1] = sAxis.y; sPlane[2] = sAxis.z;
        tPlane[0] = tAxis.x; tPlane[1] = tAxis.y; tPlane[2] = tAxis.z;
        sPlane[3] = (Point3f(0, 0, 0) - origin) * sAxis / planetRadius + 0.5f;
        tPlane[3] = (Point3f(0, 0, 0) - origin) * tAxis / planetRadius + 0.5f;

        // TODO: Multiple eclipse shadows should be rendered in a single
        // pass using multitexture.
        if (eclipseTex != NULL)
            eclipseTex->bind();
        // shadowMaskTexture->bind();
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);

        // If the ambient light level is greater than zero, reduce the
        // darkness of the shadows.
        if (ri.useTexEnvCombine)
        {
            float color[4] = { ri.ambientColor.red(), ri.ambientColor.green(),
                               ri.ambientColor.blue(), 1.0f };
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_CONSTANT_EXT);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_ADD);

            // The second texture unit has the shadow 'mask'
            glx::glActiveTextureARB(GL_TEXTURE1_ARB);
            glEnable(GL_TEXTURE_2D);
            shadowMaskTexture->bind();
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_ADD);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PREVIOUS_EXT);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
            glx::glActiveTextureARB(GL_TEXTURE0_ARB);
        }

        // Since invariance between nVidia's vertex programs and the
        // standard transformation pipeline isn't guaranteed, we have to
        // make sure to use the same transformation engine on subsequent
        // rendering passes as we did on the initial one.
        if (context.getVertexPath() != GLContext::VPath_Basic && model == NULL)
        {
            renderShadowedModelVertexShader(ri, viewFrustum,
                                           sPlane, tPlane,
                                           dir,
                                           context);
        }
        else
        {
            renderShadowedModelDefault(model, ri, viewFrustum,
                                       sPlane, tPlane,
                                       dir,
                                       ri.useTexEnvCombine,
                                       context);
        }

        if (ri.useTexEnvCombine)
        {
            // Disable second texture unit
            glx::glActiveTextureARB(GL_TEXTURE1_ARB);
            glDisable(GL_TEXTURE_2D);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glx::glActiveTextureARB(GL_TEXTURE0_ARB);

            float color[4] = { 0, 0, 0, 0 };
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        }

        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDisable(GL_BLEND);
    }
}


static void
renderEclipseShadows_Shaders(Model* model,
                             vector<Renderer::EclipseShadow>& eclipseShadows,
                             RenderInfo& ri,
                             float planetRadius,
                             Mat4f& planetMat,
                             Frustum& viewFrustum,
                             const GLContext& context)
{
    // Eclipse shadows on mesh objects aren't working yet.
    if (model != NULL)
        return;

    glEnable(GL_TEXTURE_2D);
    penumbraFunctionTexture->bind();

    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);

    float sPlanes[4][4];
    float tPlanes[4][4];
    float shadowParams[4][4];

    int n = 0;
    for (vector<Renderer::EclipseShadow>::iterator iter = eclipseShadows.begin();
         iter != eclipseShadows.end() && n < 4; iter++, n++)
    {
        Renderer::EclipseShadow shadow = *iter;

        float R2 = 0.25f;
        float umbra = shadow.umbraRadius / shadow.penumbraRadius;
        umbra = umbra * umbra;
        if (umbra < 0.0001f)
            umbra = 0.0001f;
        else if (umbra > 0.99f)
            umbra = 0.99f;

        float umbraRadius = R2 * umbra;
        float penumbraRadius = R2;
        float shadowBias = 1.0f / (1.0f - penumbraRadius / umbraRadius);
        float shadowScale = -shadowBias / umbraRadius;

        shadowParams[n][0] = shadowScale;
        shadowParams[n][1] = shadowBias;
        shadowParams[n][2] = 0.0f;
        shadowParams[n][3] = 0.0f;

        // Compute the transformation to use for generating texture
        // coordinates from the object vertices.
        Point3f origin = shadow.origin * planetMat;
        Vec3f dir = shadow.direction * planetMat;
        float scale = planetRadius / shadow.penumbraRadius;
        Vec3f axis = Vec3f(0, 1, 0) ^ dir;
        float angle = (float) acos(Vec3f(0, 1, 0) * dir);
        axis.normalize();
        Mat4f mat = Mat4f::rotation(axis, -angle);
        Vec3f sAxis = Vec3f(0.5f * scale, 0, 0) * mat;
        Vec3f tAxis = Vec3f(0, 0, 0.5f * scale) * mat;

        sPlanes[n][0] = sAxis.x;
        sPlanes[n][1] = sAxis.y;
        sPlanes[n][2] = sAxis.z;
        sPlanes[n][3] = (Point3f(0, 0, 0) - origin) * sAxis / planetRadius + 0.5f;
        tPlanes[n][0] = tAxis.x;
        tPlanes[n][1] = tAxis.y;
        tPlanes[n][2] = tAxis.z;
        tPlanes[n][3] = (Point3f(0, 0, 0) - origin) * tAxis / planetRadius + 0.5f;
    }


    VertexProcessor* vproc = context.getVertexProcessor();
    FragmentProcessor* fproc = context.getFragmentProcessor();

    vproc->enable();
    vproc->use(vp::multiShadow);

    fproc->enable();
    if (n == 1)
        fproc->use(fp::eclipseShadow1);
    else
        fproc->use(fp::eclipseShadow2);

    fproc->parameter(fp::ShadowParams0, shadowParams[0]);
    vproc->parameter(vp::TexGen_S, sPlanes[0]);
    vproc->parameter(vp::TexGen_T, tPlanes[0]);
    if (n >= 2)
    {
        fproc->parameter(fp::ShadowParams1, shadowParams[1]);
        vproc->parameter(vp::TexGen_S2, sPlanes[1]);
        vproc->parameter(vp::TexGen_T2, tPlanes[1]);
    }
    if (n >= 3)
    {
        //fproc->parameter(fp::ShadowParams2, shadowParams[2]);
        vproc->parameter(vp::TexGen_S3, sPlanes[2]);
        vproc->parameter(vp::TexGen_T3, tPlanes[2]);
    }
    if (n >= 4)
    {
        //fproc->parameter(fp::ShadowParams3, shadowParams[3]);
        vproc->parameter(vp::TexGen_S4, sPlanes[3]);
        vproc->parameter(vp::TexGen_T4, tPlanes[3]);
    }

    //vproc->parameter(vp::SunDirection, lightDir);

    lodSphere->render(context,
                      LODSphereMesh::Normals | LODSphereMesh::Multipass,
                      viewFrustum,
                      ri.pixWidth, NULL);

    vproc->disable();
    
#if 0
        float R2 = 0.25f;
        float umbra = shadow.umbraRadius / shadow.penumbraRadius;
        umbra = umbra * umbra;
        if (umbra < 0.0001f)
            umbra = 0.0001f;
        else if (umbra > 0.99f)
            umbra = 0.99f;

        float umbraRadius = R2 * umbra;
        float penumbraRadius = R2;
        float shadowBias = 1.0f / (1.0f - penumbraRadius / umbraRadius);
        float shadowScale = -shadowBias / umbraRadius;
        
        fproc->parameter(fp::ShadowParams0,
                         shadowScale, shadowBias, 0.0f, 0.0f);
        fproc->parameter(fp::AmbientColor, ri.ambientColor * ri.color);

        // Compute the transformation to use for generating texture
        // coordinates from the object vertices.
        Point3f origin = shadow.origin * planetMat;
        Vec3f dir = shadow.direction * planetMat;
        float scale = planetRadius / shadow.penumbraRadius;
        Vec3f axis = Vec3f(0, 1, 0) ^ dir;
        float angle = (float) acos(Vec3f(0, 1, 0) * dir);
        axis.normalize();
        Mat4f mat = Mat4f::rotation(axis, -angle);
        Vec3f sAxis = Vec3f(0.5f * scale, 0, 0) * mat;
        Vec3f tAxis = Vec3f(0, 0, 0.5f * scale) * mat;

        float sPlane[4] = { 0, 0, 0, 0 };
        float tPlane[4] = { 0, 0, 0, 0 };
        sPlane[0] = sAxis.x; sPlane[1] = sAxis.y; sPlane[2] = sAxis.z;
        tPlane[0] = tAxis.x; tPlane[1] = tAxis.y; tPlane[2] = tAxis.z;
        sPlane[3] = (Point3f(0, 0, 0) - origin) * sAxis / planetRadius + 0.5f;
        tPlane[3] = (Point3f(0, 0, 0) - origin) * tAxis / planetRadius + 0.5f;

        renderShadowedModelVertexShader(ri, viewFrustum,
                                        sPlane, tPlane,
                                        dir,
                                        context);

    }
#endif

    fproc->disable();

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_BLEND);
}


// Approximate ring shadows using texture coordinate generation.  Vertex
// shaders are required for the real thing.
static void
renderRingShadows(Model* model,
                  const RingSystem& rings,
                  const Vec3f& sunDir,
                  RenderInfo& ri,
                  float planetRadius,
                  Mat4f& planetMat,
                  Frustum& viewFrustum,
                  const GLContext& context)
{
    // Compute the transformation to use for generating texture
    // coordinates from the object vertices.
    float ringWidth = rings.outerRadius - rings.innerRadius;
    Vec3f v = ri.sunDir_obj ^ Vec3f(0, 1, 0);
    v.normalize();
    Vec3f dir = v ^ ri.sunDir_obj;
    dir.normalize();
    Vec3f u = v ^ Vec3f(0, 1, 0);
    u.normalize();
    Vec3f origin = u * (rings.innerRadius / planetRadius);
    float s = ri.sunDir_obj.y;
    float scale = (abs(s) < 0.001f) ? 1000.0f : 1.0f / s;
    scale *= planetRadius / ringWidth;
    Vec3f sAxis = -(dir * scale);

    float sPlane[4] = { 0, 0, 0, 0 };
    float tPlane[4] = { 0, 0, 0, 0.5f };
    sPlane[0] = sAxis.x;
    sPlane[1] = sAxis.y;
    sPlane[2] = sAxis.z;
    sPlane[3] = (origin * sAxis);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

    // If the ambient light level is greater than zero, reduce the
    // darkness of the shadows.
    if (ri.useTexEnvCombine)
    {
        float color[4] = { ri.ambientColor.red(), ri.ambientColor.green(),
                           ri.ambientColor.blue(), 1.0f };
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_CONSTANT_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_ADD);
    }

    float bc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER_ARB);

    renderShadowedModelDefault(model, ri, viewFrustum,
                               sPlane, tPlane, Vec3f(0.0f, 0.0f, 0.0f), false,
                               context);

    if (ri.useTexEnvCombine)
    {
        float color[4] = { 0, 0, 0, 0 };
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_BLEND);
}


static void
renderRingShadowsVS(Model* model,
                    const RingSystem& rings,
                    const Vec3f& sunDir,
                    RenderInfo& ri,
                    float planetRadius,
                    float oblateness,
                    Mat4f& planetMat,
                    Frustum& viewFrustum,
                    const GLContext& context)
{
    // Compute the transformation to use for generating texture
    // coordinates from the object vertices.
    float ringWidth = rings.outerRadius - rings.innerRadius;
    float s = ri.sunDir_obj.y;
    float scale = (abs(s) < 0.001f) ? 1000.0f : 1.0f / s;

    if (abs(s) > 1.0f - 1.0e-4f)
    {
        // Planet is illuminated almost directly from above, so
        // no ring shadow will be cast on the planet.  Conveniently
        // avoids some potential division by zero when ray-casting.
        return;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

    // If the ambient light level is greater than zero, reduce the
    // darkness of the shadows.
    float color[4] = { ri.ambientColor.red(), ri.ambientColor.green(),
                       ri.ambientColor.blue(), 1.0f };
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_CONSTANT_EXT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_ADD);

    // Tweak the texture--set clamp to border and a border color with
    // a zero alpha.  If a graphics card doesn't support clamp to border,
    // it doesn't get to play.  It's possible to get reasonable behavior
    // by turning off mipmaps and assuming transparent rows of pixels for
    // the top and bottom of the ring textures . . . maybe later.
    float bc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER_ARB);

    // Ring shadows look strange if they're always completely black.  Vary
    // the darkness of the shadow based on the angle between the sun and the
    // ring plane.  There's some justification for this--the larger the angle
    // between the sun and the ring plane (normal), the more ring material
    // there is to travel through.
    //float alpha = (1.0f - abs(ri.sunDir_obj.y)) * 1.0f;
    // ...but, images from Cassini are showing very dark ring shadows, so we'll
    // go with that.
    float alpha = 1.0f;

    VertexProcessor* vproc = context.getVertexProcessor();
    assert(vproc != NULL);

    vproc->enable();
    vproc->use(vp::ringShadow);
    vproc->parameter(vp::SunDirection, ri.sunDir_obj);
    vproc->parameter(vp::DiffuseColor, 1, 1, 1, alpha); // color = white
    vproc->parameter(vp::TexGen_S,
                     rings.innerRadius / planetRadius,
                     1.0f / (ringWidth / planetRadius),
                     0.0f, 0.5f);
    vproc->parameter(vp::TexGen_T, scale, 0, 0, 0);
    lodSphere->render(context, LODSphereMesh::Multipass,
                      viewFrustum, ri.pixWidth, NULL);
    vproc->disable();

    // Restore the texture combiners
    if (ri.useTexEnvCombine)
    {
        float color[4] = { 0, 0, 0, 0 };
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_BLEND);
}


void Renderer::renderLocations(const vector<Location*>& locations,
                               const Quatf& cameraOrientation,
                               const Point3f& position,
                               const Quatf& orientation,
                               float scale)
{
    if (font == NULL)
        return;

    double winX, winY, winZ;
    int view[4] = { 0, 0, 0, 0 };
    view[0] = -windowWidth / 2;
    view[1] = -windowHeight / 2;
    view[2] = windowWidth;
    view[3] = windowHeight;

    Vec3f viewNormal = Vec3f(0.0f, 0.0f, -1.0f) *
        cameraOrientation.toMatrix3();

    double modelview[16];
    double projection[16];
    glGetDoublev(GL_MODELVIEW_MATRIX, modelview);
    glGetDoublev(GL_PROJECTION_MATRIX, projection);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    font->bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_LIGHTING);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, windowWidth, 0, windowHeight, 1.0f, -1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Render the labels very close to the near plane with z=-0.999f.  In fact,
    // z=-1.0f should work, but I'm concerned that some OpenGL implementations
    // might clip things placed right on the near plane.
    glTranslatef(GLfloat((int) (windowWidth / 2)),
                 GLfloat((int) (windowHeight / 2)), -0.999f);

    Ellipsoidf ellipsoid(position, Vec3f(scale, scale, scale));

    Point3f origin(0.0f, 0.0f, 0.0f);
    float iScale = 1.0f / scale;
    Mat3f mat = orientation.toMatrix3();

    for (vector<Location*>::const_iterator iter = locations.begin();
         iter != locations.end(); iter++)
    {
        if ((*iter)->getFeatureType() & locationFilter)
        {
            Vec3f off = (*iter)->getPosition();
            Vec3f off_t = off * mat;
            Point3f wpos(position + off_t * 1.0001f);

            float effSize = (*iter)->getImportance();
            if (effSize < 0.0f)
                effSize = (*iter)->getSize();
            float pixSize = effSize / (wpos.distanceFromOrigin() * pixelSize);
            
            if (pixSize > minFeatureSize &&
                (wpos - origin) * viewNormal > 0.0f)
            {
                float r = off_t.length();
                if (r < scale * 0.99f)
                    wpos = position + off_t * (scale * 1.01f / r);
                
                float t = 0.0f;
                bool hit = testIntersection(Ray3f(origin, wpos - origin),
                                            ellipsoid, t);
                if (!hit || t >= 1.0f)
                {
                    if (gluProject(off.x * iScale, off.y * iScale, off.z * iScale,
                                   modelview,
                                   projection,
                                   (const GLint*) view,
                                   &winX, &winY, &winZ) != GL_FALSE)
                    {
                        glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
                        glPushMatrix();
                        glTranslatef((int) winX + PixelOffset,
                                     (int) winY + PixelOffset,
                                     0.0f);
                        font->render((*iter)->getName());
                        glPopMatrix();
                    }
                }
            }
        }
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}


void Renderer::renderObject(Point3f pos,
                            float distance,
                            double now,
                            Quatf cameraOrientation,
                            float nearPlaneDistance,
                            float farPlaneDistance,
                            Vec3f sunDirection,
                            Color sunColor,
                            RenderProperties& obj)
{
    RenderInfo ri;

    float altitude = distance - obj.radius;
    float discSizeInPixels = obj.radius /
        (max(nearPlaneDistance, altitude) * pixelSize);

    // Enable depth buffering
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    glDisable(GL_BLEND);

    // Get the textures . . .
    if (obj.surface->baseTexture.tex[textureResolution] != InvalidResource)
        ri.baseTex = obj.surface->baseTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::ApplyBumpMap) != 0 &&
        context->bumpMappingSupported() &&
        obj.surface->bumpTexture.tex[textureResolution] != InvalidResource)
        ri.bumpTex = obj.surface->bumpTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::ApplyNightMap) != 0 &&
        (renderFlags & ShowNightMaps) != 0)
        ri.nightTex = obj.surface->nightTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::SeparateSpecularMap) != 0)
        ri.glossTex = obj.surface->specularTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::ApplyOverlay) != 0)
        ri.overlayTex = obj.surface->overlayTexture.find(textureResolution);

    // Apply the modelview transform for the object
    glPushMatrix();
    glTranslate(pos);
    glRotate(~obj.orientation);

    // Apply a scale factor which depends on the size of the planet and
    // its oblateness.  Since the oblateness is usually quite
    // small, the potentially nonuniform scale factor shouldn't mess up
    // the lighting calculations enough to be noticeable.
    // TODO:  Figure out a better way to render ellipsoids than applying
    // a nonunifom scale factor to a sphere . . . it makes me nervous.
    float radius = obj.radius;
    Vec3f semiMajorAxes(radius, radius * (1.0f - obj.oblateness), radius);
    glScale(semiMajorAxes);

    Mat4f planetMat = (~obj.orientation).toMatrix4();
    ri.sunDir_eye = sunDirection;
    ri.sunDir_eye.normalize();
    ri.sunDir_obj = ri.sunDir_eye * planetMat;
    ri.eyeDir_obj = (Point3f(0, 0, 0) - pos) * planetMat;
    ri.eyeDir_obj.normalize();
    ri.eyePos_obj = Point3f(-pos.x / radius,
                            -pos.y / ((1.0f - obj.oblateness) * radius),
                            -pos.z / radius) * planetMat;
    
    ri.orientation = cameraOrientation;

    ri.pixWidth = discSizeInPixels;

    // Set up the colors
    if (ri.baseTex == NULL ||
        (obj.surface->appearanceFlags & Surface::BlendTexture) != 0)
    {
        ri.color = obj.surface->color;
    }

    ri.sunColor = sunColor;
    ri.ambientColor = ambientColor * ri.sunColor;
    ri.hazeColor = obj.surface->hazeColor;
    ri.specularColor = obj.surface->specularColor;
    ri.specularPower = obj.surface->specularPower;
    ri.useTexEnvCombine = context->getRenderPath() != GLContext::GLPath_Basic;

    // See if the surface should be lit
    bool lit = (obj.surface->appearanceFlags & Surface::Emissive) == 0;

    // Set up the light source for the sun
    glLightDirection(GL_LIGHT0, ri.sunDir_obj);

    // RANT ALERT!
    // This sucks, but it's necessary.  glScale is used to scale a unit
    // sphere up to planet size.  Since normals are transformed by the
    // inverse transpose of the model matrix, this means they end up
    // getting scaled by a factor of 1.0 / planet radius (in km).  This
    // has terrible effects on lighting: the planet appears almost
    // completely dark.  To get around this, the GL_rescale_normal
    // extension was introduced and eventually incorporated into into the
    // OpenGL 1.2 standard.  Of course, not everyone implemented this
    // incredibly simple and essential little extension.  Microsoft is
    // notorious for half-assed support of OpenGL, but 3dfx should have
    // known better: no Voodoo 1/2/3 drivers seem to support this
    // extension.  The following is an attempt to get around the problem by
    // scaling the light brightness by the planet radius.  According to the
    // OpenGL spec, this should work fine, as clamping of colors to [0, 1]
    // occurs *after* lighting.  It works fine on my GeForce3 when I
    // disable EXT_rescale_normal, but I'm not certain whether other
    // drivers are as well behaved as nVidia's.
    //
    // Addendum: Unsurprisingly, using color values outside [0, 1] produces
    // problems on Savage4 cards.
    //
    if (useRescaleNormal)
    {
        glLightColor(GL_LIGHT0, GL_DIFFUSE, ri.sunColor);
        glLightColor(GL_LIGHT0, GL_SPECULAR, ri.sunColor);
    }
    else
    {
        glLightColor(GL_LIGHT0, GL_DIFFUSE,
                     Vec3f(ri.sunColor.red(), ri.sunColor.green(), ri.sunColor.blue()) * radius);
    }
    glEnable(GL_LIGHT0);

    // Compute the inverse model/view matrix
    Mat4f invMV = (cameraOrientation.toMatrix4() *
                   Mat4f::translation(Point3f(-pos.x, -pos.y, -pos.z)) *
                   planetMat *
                   Mat4f::scaling(1.0f / radius));

    // Transform the frustum into object coordinates using the
    // inverse model/view matrix.
    Frustum viewFrustum(degToRad(fov),
                        (float) windowWidth / (float) windowHeight,
                        nearPlaneDistance, farPlaneDistance);
    viewFrustum.transform(invMV);

    // Temporary hack until we fix culling for ringed planets
    if (obj.rings != NULL)
    {
        if (ri.pixWidth > 5000)
            ri.pixWidth = 5000;
    }

    Model* model = NULL;
    if (obj.model == InvalidResource)
    {
        // A null model indicates that this body is a sphere
        if (lit)
        {
            switch (context->getRenderPath())
            {
            case GLContext::GLPath_NV30:
                renderSphere_FP_VP(ri, viewFrustum, *context);
                break;

            case GLContext::GLPath_NvCombiner_ARBVP:
            case GLContext::GLPath_NvCombiner_NvVP:
                renderSphere_Combiners_VP(ri, viewFrustum, *context);
                break;

            case GLContext::GLPath_NvCombiner:
                renderSphere_Combiners(ri, viewFrustum, *context);
                break;

            case GLContext::GLPath_DOT3_ARBVP:
                renderSphere_DOT3_VP(ri, viewFrustum, *context);
                break;

            default:
                renderSphereDefault(ri, viewFrustum, true, *context);
            }
        }
        else
        {
            renderSphereDefault(ri, viewFrustum, false, *context);
        }
    }
    else
    {
        // This is a model loaded from a file
        model = GetModelManager()->find(obj.model);
        if (model != NULL)
            renderModelDefault(model, ri, lit);
    }

    if (obj.rings != NULL && distance <= obj.rings->innerRadius)
    {
        renderRings(*obj.rings, ri, radius, obj.oblateness,
                    textureResolution,
                    context->getMaxTextures() > 1 &&
                    (renderFlags & ShowRingShadows) != 0 && lit,
                    *context,
                    detailOptions.ringSystemSections);
    }

    if (obj.atmosphere != NULL)
    {
        Atmosphere* atmosphere = const_cast<Atmosphere *>(obj.atmosphere);

        // Compute the apparent thickness in pixels of the atmosphere.
        // If it's only one pixel thick, it can look quite unsightly
        // due to aliasing.  To avoid popping, we gradually fade in the
        // atmosphere as it grows from two to three pixels thick.
        float fade;
        float thicknessInPixels = 0.0f;
        if (distance - radius > 0.0f)
        {
            thicknessInPixels = atmosphere->height /
                ((distance - radius) * pixelSize);
            fade = clamp(thicknessInPixels - 2);
        }
        else
        {
            fade = 1.0f;
        }

        if (fade > 0 && (renderFlags & ShowAtmospheres) != 0)
        {
            glPushMatrix();
            glLoadIdentity();
            glDisable(GL_LIGHTING);
            glDisable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

#ifdef OLD_ATMOSPHERE
            renderAtmosphere(*atmosphere,
                             pos * (~cameraOrientation).toMatrix3(),
                             radius,
                             ri.sunDir_eye * (~cameraOrientation).toMatrix3(),
                             ri.ambientColor,
                             fade,
                             lit);
#else
            glRotate(cameraOrientation);
            renderEllipsoidAtmosphere(*atmosphere,
                                      pos,
                                      obj.orientation,
                                      semiMajorAxes,
                                      ri.sunDir_eye,
                                      ri.ambientColor,
                                      thicknessInPixels,
                                      lit);
#endif // OLD_ATMOSPHERE
            glEnable(GL_TEXTURE_2D);
            glPopMatrix();
        }

        // If there's a cloud layer, we'll render it now.
        Texture* cloudTex = NULL;
        if ((renderFlags & ShowCloudMaps) != 0 &&
            atmosphere->cloudTexture.tex[textureResolution] != InvalidResource)
            cloudTex = atmosphere->cloudTexture.find(textureResolution);

        if (cloudTex != NULL)
        {
            glPushMatrix();
            float cloudScale = 1.0f + atmosphere->cloudHeight / radius;
            glScalef(cloudScale, cloudScale, cloudScale);

            // If we're beneath the cloud level, render the interior of
            // the cloud sphere.
            if (distance - radius < atmosphere->cloudHeight)
                glFrontFace(GL_CW);

            float texOffset = (float) (-pfmod(now * atmosphere->cloudSpeed / (2 * PI), 1.0));

            if (atmosphere->cloudSpeed != 0.0f)
            {
                // Make the clouds appear to rotate above the surface of
                // the planet.  This is easier to do with the texture
                // matrix than the model matrix because changing the
                // texture matrix doesn't require us to compute a second
                // set of model space rendering parameters.
                glMatrixMode(GL_TEXTURE);
                glTranslatef(texOffset, 0.0f, 0.0f);
                glMatrixMode(GL_MODELVIEW);
            }

            glEnable(GL_LIGHTING);
            glDepthMask(GL_FALSE);
            cloudTex->bind();
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(1, 1, 1, 1);

            if (lit)
            {
                VertexProcessor* vproc = context->getVertexProcessor();
                if (vproc != NULL)
                {
                    vproc->enable();
                    vproc->use(vp::diffuseTexOffset);
                    vproc->parameter(vp::DiffuseColor, ri.sunColor * ri.color);
                    vproc->parameter(vp::AmbientColor, ri.ambientColor * ri.color);
                    vproc->parameter(vp::TextureTranslation,
                                     texOffset, 0.0f, 0.0f, 0.0f);
                }

                lodSphere->render(*context,
                                  LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                                  viewFrustum,
                                  ri.pixWidth,
                                  cloudTex);

                if (vproc != NULL)
                    vproc->disable();
            }
            else
            {
                glDisable(GL_LIGHTING);
                lodSphere->render(*context,
                                  LODSphereMesh::Normals | LODSphereMesh::TexCoords0,
                                  viewFrustum,
                                  ri.pixWidth,
                                  cloudTex);
                glEnable(GL_LIGHTING);
            }

            // Reset the texture matrix
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);

            glDepthMask(GL_TRUE);
            glFrontFace(GL_CCW);
            glPopMatrix();
        }
    }

    if (obj.eclipseShadows != NULL &&
        (obj.surface->appearanceFlags & Surface::Emissive) == 0)
    {
#if 1
        // renderEclipseShadows_Shaders() still needs some more work.
        if (context->getVertexProcessor() != NULL &&
            context->getFragmentProcessor() != NULL)
        {
            renderEclipseShadows_Shaders(model,
                                         *obj.eclipseShadows,
                                         ri,
                                         radius, planetMat, viewFrustum,
                                         *context);
        }
        else
#endif
        {
            renderEclipseShadows(model,
                                 *obj.eclipseShadows,
                                 ri,
                                 radius, planetMat, viewFrustum,
                                 *context);
        }
    }

    if (obj.rings != NULL &&
        (obj.surface->appearanceFlags & Surface::Emissive) == 0 &&
        (renderFlags & ShowRingShadows) != 0)
    {
        Texture* ringsTex = obj.rings->texture.find(textureResolution);
        if (ringsTex != NULL)
        {
            Vec3f sunDir = pos - Point3f(0, 0, 0);
            sunDir.normalize();

            ringsTex->bind();

            if (useClampToBorder &&
                context->getVertexPath() != GLContext::VPath_Basic)
            {
                renderRingShadowsVS(model,
                                    *obj.rings,
                                    sunDir,
                                    ri,
                                    radius, obj.oblateness,
                                    planetMat, viewFrustum,
                                    *context);
            }
            else if (useClampToBorder)
            {
                // Approximate ring shadow rendering . . . turned off
                // for now.  Vertex shaders give you the real thing.
#if 0
                renderRingShadows(model,
                                  *obj.rings,
                                  sunDir,
                                  ri,
                                  radius, planetMat, viewFrustum,
                                  context);
#endif
            }
        }
    }

    if (obj.rings != NULL && distance > obj.rings->innerRadius)
    {
        renderRings(*obj.rings, ri, radius, obj.oblateness,
                    textureResolution,
                    (context->hasMultitexture() &&
                     (renderFlags & ShowRingShadows) != 0 && lit),
                    *context,
                    detailOptions.ringSystemSections);
    }

    if (obj.locations != NULL && (labelMode & LocationLabels) != 0)
        renderLocations(*obj.locations,
                        cameraOrientation,
                        pos, obj.orientation, radius);

    glPopMatrix();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
}


bool Renderer::testEclipse(const Body& receiver, const Body& caster,
                           double now)
{
    // Ignore situations where the shadow casting body is much smaller than
    // the receiver, as these shadows aren't likely to be relevant.  Also,
    // ignore eclipses where the caster is not an ellipsoid, since we can't
    // generate correct shadows in this case.
    if (caster.getRadius() * 100 >= receiver.getRadius() &&
        caster.getClassification() != Body::Invisible &&
        caster.getModel() == InvalidResource)
    {
        // All of the eclipse related code assumes that both the caster
        // and receiver are spherical.  Irregular receivers will work more
        // or less correctly, but casters that are sufficiently non-spherical
        // will produce obviously incorrect shadows.  Another assumption we
        // make is that the distance between the caster and receiver is much
        // less than the distance between the sun and the receiver.  This
        // approximation works everywhere in the solar system, and likely
        // works for any orbitally stable pair of objects orbiting a star.
        Point3d posReceiver = receiver.getHeliocentricPosition(now);
        Point3d posCaster = caster.getHeliocentricPosition(now);

        const Star* sun = receiver.getSystem()->getStar();
        assert(sun != NULL);
        double distToSun = posReceiver.distanceFromOrigin();
        float appSunRadius = (float) (sun->getRadius() / distToSun);

        Vec3d dir = posCaster - posReceiver;
        double distToCaster = dir.length() - receiver.getRadius();
        float appOccluderRadius = (float) (caster.getRadius() / distToCaster);

        // The shadow radius is the radius of the occluder plus some additional
        // amount that depends upon the apparent radius of the sun.  For
        // a sun that's distant/small and effectively a point, the shadow
        // radius will be the same as the radius of the occluder.
        float shadowRadius = (1 + appSunRadius / appOccluderRadius) *
            caster.getRadius();

        // Test whether a shadow is cast on the receiver.  We want to know
        // if the receiver lies within the shadow volume of the caster.  Since
        // we're assuming that everything is a sphere and the sun is far
        // away relative to the caster, the shadow volume is a
        // cylinder capped at one end.  Testing for the intersection of a
        // singly capped cylinder is as simple as checking the distance
        // from the center of the receiver to the axis of the shadow cylinder.
        // If the distance is less than the sum of the caster's and receiver's
        // radii, then we have an eclipse.
        float R = receiver.getRadius() + shadowRadius;
        double dist = distance(posReceiver,
                               Ray3d(posCaster, posCaster - Point3d(0, 0, 0)));
        if (dist < R)
        {
            Vec3d sunDir = posCaster - Point3d(0, 0, 0);
            sunDir.normalize();

            Renderer::EclipseShadow shadow;
            shadow.origin = Point3f((float) dir.x,
                                    (float) dir.y,
                                    (float) dir.z);
            shadow.direction = Vec3f((float) sunDir.x,
                                     (float) sunDir.y,
                                     (float) sunDir.z);
            shadow.penumbraRadius = shadowRadius;
            shadow.umbraRadius = caster.getRadius() *
                (appOccluderRadius - appSunRadius) / appOccluderRadius;
            eclipseShadows.insert(eclipseShadows.end(), shadow);

            return true;
        }
    }

    return false;
}


void Renderer::renderPlanet(Body& body,
                            Point3f pos,
                            Vec3f sunDirection,
                            float distance,
                            float appMag,
                            double now,
                            Quatf orientation,
                            float nearPlaneDistance,
                            float farPlaneDistance)
{
    float altitude = distance - body.getRadius();
    float discSizeInPixels = body.getRadius() /
        (max(nearPlaneDistance, altitude) * pixelSize);

    if (discSizeInPixels > 1)
    {
        RenderProperties rp;

        if (displayedSurface.empty())
        {
            rp.surface = const_cast<Surface*>(&body.getSurface());
        }
        else
        {
            rp.surface = body.getAlternateSurface(displayedSurface);
            if (rp.surface == NULL)
                rp.surface = const_cast<Surface*>(&body.getSurface());
        }
        rp.atmosphere = body.getAtmosphere();
        rp.rings = body.getRings();
        rp.radius = body.getRadius();
        rp.oblateness = body.getOblateness();
        rp.model = body.getModel();

        // Compute the orientation of the planet before axial rotation
        Quatd q = body.getEclipticalToEquatorial(now);
        rp.orientation = Quatf((float) q.w, (float) q.x, (float) q.y,
                               (float) q.z);

        double rotation = 0.0;
        // Watch out for the precision limits of floats when computing
        // rotation . . .
        {
            RotationElements re = body.getRotationElements();
            double rotations = (now - re.epoch) / (double) re.period;
            double wholeRotations = floor(rotations);
            double remainder = rotations - wholeRotations;

            // Add an extra half rotation because of the convention in all
            // planet texture maps where zero deg long. is in the middle of
            // the texture.
            remainder += 0.5;

            rotation = remainder * 2 * PI + re.offset;
        }
        rp.orientation.yrotate((float) -rotation);
        rp.orientation = body.getOrientation() * rp.orientation;

        Color sunColor(1.0f, 1.0f, 1.0f);
        {
            // If the star is sufficiently cool, change the light color
            // from white.  Though our sun appears yellow, we still make
            // it and all hotter stars emit white light, as this is the
            // 'natural' light to which our eyes are accustomed.  We also
            // assign a slight bluish tint to light from O and B type stars,
            // though these will almost never have planets for their light
            // to shine upon.
            PlanetarySystem* system = body.getSystem();
            if (system != NULL)
            {
                const Star* sun = system->getStar();
                switch (sun->getStellarClass().getSpectralClass())
                {
                case StellarClass::Spectral_O:
                    sunColor = Color(0.8f, 0.8f, 1.0f);
                    break;
                case StellarClass::Spectral_B:
                    sunColor = Color(0.9f, 0.9f, 1.0f);
                    break;
                case StellarClass::Spectral_K:
                    sunColor = Color(1.0f, 0.9f, 0.8f);
                    break;
                case StellarClass::Spectral_M:
                    sunColor = Color(1.0f, 0.7f, 0.7f);
                    break;
                case StellarClass::Spectral_R:
                case StellarClass::Spectral_S:
                case StellarClass::Spectral_N:
                case StellarClass::Spectral_C:
                    sunColor = Color(1.0f, 0.4f, 0.4f);
                    break;
                default:
                    // Default case to keep gcc from compaining about unhandled
                    // switch values.
                    break;
                }
            }
        }

        rp.locations = body.getLocations();
        if (rp.locations != NULL && (labelMode & LocationLabels) != 0)
            body.computeLocations();

        // Calculate eclipse circumstances
        if ((renderFlags & ShowEclipseShadows) != 0 &&
            body.getClassification() != Body::Invisible)
        {
            PlanetarySystem* system = body.getSystem();
            if (system != NULL)
            {
                // Clear out the list of eclipse shadows
                eclipseShadows.clear();

                if (system->getPrimaryBody() == NULL &&
                    body.getSatellites() != NULL)
                {
                    // The body is a planet.  Check for eclipse shadows
                    // from all of its satellites.
                    PlanetarySystem* satellites = body.getSatellites();
                    if (satellites != NULL)
                    {
                        int nSatellites = satellites->getSystemSize();
                        for (int i = 0; i < nSatellites; i++)
                            testEclipse(body, *satellites->getBody(i), now);
                    }
                }
                else if (system->getPrimaryBody() != NULL)
                {
                    // The body is a moon.  Check for eclipse shadows from
                    // the parent planet and all satellites in the system.
                    Body* planet = system->getPrimaryBody();
                    testEclipse(body, *planet, now);

                    int nSatellites = system->getSystemSize();
                    for (int i = 0; i < nSatellites; i++)
                    {
                        if (system->getBody(i) != &body)
                            testEclipse(body, *system->getBody(i), now);
                    }
                }

                if (eclipseShadows.size() != 0)
                    rp.eclipseShadows = &eclipseShadows;
            }
        }

        renderObject(pos, distance, now,
                     orientation, nearPlaneDistance, farPlaneDistance,
                     sunDirection, sunColor, rp);
    }

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    renderBodyAsParticle(pos,
                         appMag,
                         faintestPlanetMag,
                         discSizeInPixels,
                         body.getSurface().color,
                         orientation,
                         (nearPlaneDistance + farPlaneDistance) / 2.0f,
                         false);
}


void Renderer::renderStar(const Star& star,
                          Point3f pos,
                          float distance,
                          float appMag,
                          Quatf orientation,
                          double now,
                          float nearPlaneDistance,
                          float farPlaneDistance)
{
    Color color = star.getStellarClass().getApparentColor();
    float radius = star.getRadius();
    float discSizeInPixels = radius / (distance * pixelSize);

    if (discSizeInPixels > 1)
    {
        Surface surface;
        Atmosphere atmosphere;
        RenderProperties rp;

        surface.color = color;
        ResourceHandle tex;
        switch (star.getStellarClass().getSpectralClass())
        {
        case StellarClass::Spectral_O:
        case StellarClass::Spectral_B:
            tex = starTexB;
            break;
        case StellarClass::Spectral_A:
        case StellarClass::Spectral_F:
            tex = starTexA;
            break;
        case StellarClass::Spectral_G:
        case StellarClass::Spectral_K:
            tex = starTexG;
            break;
        case StellarClass::Spectral_M:
        case StellarClass::Spectral_R:
        case StellarClass::Spectral_S:
        case StellarClass::Spectral_N:
        case StellarClass::Spectral_C:
            tex = starTexM;
            break;
        case StellarClass::Spectral_L:
        case StellarClass::Spectral_T:
            tex = starTexL;
            break;
        default:
            tex = starTexA;
            break;
        }
        surface.baseTexture = MultiResTexture(tex, tex, tex);
        surface.appearanceFlags |= Surface::ApplyBaseTexture;
        surface.appearanceFlags |= Surface::Emissive;

        atmosphere.height = radius * CoronaHeight;
        atmosphere.lowerColor = color;
        atmosphere.upperColor = color;
        atmosphere.skyColor = color;

        rp.surface = &surface;
        rp.atmosphere = &atmosphere;
        rp.rings = NULL;
        rp.radius = star.getRadius();
        rp.oblateness = 0.0f;
        rp.model = InvalidResource;

        double rotation = 0.0;
        // Watch out for the precision limits of floats when computing
        // rotation . . .
        {
            double period = star.getRotationPeriod();
            double rotations = now / period;
            double remainder = rotations - floor(rotations);
            rotation = remainder * 2 * PI;
        }
        rp.orientation = Quatf(1.0f);
        rp.orientation.yrotate((float) -rotation);

        renderObject(pos, distance, now,
                     orientation, nearPlaneDistance, farPlaneDistance,
                     Vec3f(1.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f), rp);

        glEnable(GL_TEXTURE_2D);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    renderBodyAsParticle(pos,
                         appMag,
                         faintestMag,
                         discSizeInPixels,
                         color,
                         orientation,
                         (nearPlaneDistance + farPlaneDistance) / 2.0f,
                         true);
}


static const int MaxCometTailPoints = 20;
static const int CometTailSlices = 16;
struct CometTailVertex
{
    Point3f point;
    Vec3f normal;
    Point3f paraboloidPoint;
    float brightness;
};

static CometTailVertex cometTailVertices[CometTailSlices * MaxCometTailPoints];

static void ProcessCometTailVertex(const CometTailVertex& v,
                                   const Vec3f& viewDir)
{
    float shade = abs(viewDir * v.normal * v.brightness);
    glColor4f(0.0f, 0.5f, 1.0f, shade);
    glVertex(v.point);
}

static void ProcessCometTailVertex(const CometTailVertex& v,
                                   const Point3f& cameraPos)
{
    Vec3f viewDir = v.point - cameraPos;
    viewDir.normalize();
    float shade = abs(viewDir * v.normal * v.brightness);
    glColor4f(0.0f, 0.5f, 1.0f, shade);
    glVertex(v.point);
}


static void ProcessCometTailVertex(const CometTailVertex& v,
                                   const Point3f& eyePos_obj,
                                   float b,
                                   float h)
{
    float shade = 0.0f;
    Vec3f R = v.paraboloidPoint - eyePos_obj;
    float c0 = b * (square(eyePos_obj.x) + square(eyePos_obj.y)) + eyePos_obj.z;
    float c1 = 2 * b * (R.x * eyePos_obj.x + R.y * eyePos_obj.y) - R.z;
    float c2 = b * (square(R.x) + square(R.y));

    float disc = square(c1) - 4 * c0 * c2;

    if (disc < 0.0f)
    {
        shade = 0.0f;
    }
    else
    {
        disc = (float) sqrt(disc);
        float s = 1.0f / (2 * c2);
        float t0 = (h - eyePos_obj.z) / R.z;
        float t1 = (c1 - disc) * s;
        float t2 = (c1 + disc) * s;
        float u0 = max(t0, 0.0f);
        float u1, u2;

        if (R.z < 0.0f)
        {
            u1 = max(t1, t0);
            u2 = max(t2, t0);
        }
        else
        {
            u1 = min(t1, t0);
            u2 = min(t2, t0);
        }
        u1 = max(0.0f, u1);
        u2 = max(0.0f, u2);

        shade = u2 - u1;
    }

    glColor4f(0.0f, 0.5f, 1.0f, shade);
    glVertex(v.point);
}


void Renderer::renderCometTail(const Body& body,
                               Point3f pos,
                               Vec3f sunDirection,
                               float distance,
                               float appMag,
                               double now,
                               Quatf orientation,
                               float nearPlaneDistance,
                               float farPlaneDistance)
{
    Point3f cometPoints[MaxCometTailPoints];
    Point3d pos0 = body.getOrbit()->positionAtTime(now);
    Point3d pos1 = body.getOrbit()->positionAtTime(now - 0.01);
    Vec3d vd = pos1 - pos0;
    double t = now;
    float f = 1.0e15f;
    int nSteps = MaxCometTailPoints;
    float dt = 10000000.0f / (nSteps * (float) vd.length() * 100.0f);
    // float dt = 0.1f;

    int i;
#if 0
    for (i = 0; i < MaxCometTailPoints; i++)
    {
        Vec3d pd = body.getOrbit()->positionAtTime(t) - pos0;
        Point3f p = Point3f((float) pd.x, (float) pd.y, (float) pd.z);
        Vec3f r(p.x + (float) pos0.x,
                p.y + (float) pos0.y,
                p.z + (float) pos0.z);
        float distance = r.length();
            
        Vec3f a = r * ((1 / square(distance)) * f * dt);
        Vec3f dx = a * (square(i * dt) * 0.5f);

        cometPoints[i] = p + dx;

        t -= dt;
    }
#endif
    // Compute a rough estimate of the visible length of the dust tail
    float dustTailLength = (1.0e8f / (float) pos0.distanceFromOrigin()) *
        (body.getRadius() / 5.0f) * 1.0e7f;
    float dustTailRadius = dustTailLength * 0.1f;
    float comaRadius = dustTailRadius * 0.5f;

    Point3f origin(0, 0, 0);
    {
        Point3d pd = body.getOrbit()->positionAtTime(t);
        Point3f p = Point3f((float) pd.x, (float) pd.y, (float) pd.z);
        Vec3f sunDir = p - Point3f(0, 0, 0);
        sunDir.normalize();
        origin -= sunDir * (body.getRadius() * 100);
    }

    for (i = 0; i < MaxCometTailPoints; i++)
    {
        float alpha = (float) i / (float) MaxCometTailPoints;
        alpha = alpha * alpha;
        
        Point3d pd = body.getOrbit()->positionAtTime(t);
        Point3f p = Point3f((float) pd.x, (float) pd.y, (float) pd.z);
        Vec3f sunDir = p - Point3f(0, 0, 0);
        sunDir.normalize();
        
        cometPoints[i] = origin + sunDir * (dustTailLength * alpha);
    }

    // We need three axes to define the coordinate system for rendering the
    // comet.  The first axis is the velocity.  We choose the second one
    // based on the orientation of the comet.  And the third is just the cross
    // product of the first and second axes.
    Quatd qd = body.getEclipticalToEquatorial(t);
    Quatf q((float) qd.w, (float) qd.x, (float) qd.y, (float) qd.z);
    Vec3f v = cometPoints[1] - cometPoints[0];
    v.normalize();
    Vec3f u0 = Vec3f(0, 1, 0) * q.toMatrix3();
    Vec3f u1 = Vec3f(1, 0, 0) * q.toMatrix3();
    Vec3f u;
    if (abs(u0 * v) < abs(u1 * v))
        u = v ^ u0;
    else
        u = v ^ u1;
    u.normalize();
    Vec3f w = u ^ v;

    glColor4f(0.0f, 1.0f, 1.0f, 0.5f);
    glPushMatrix();
    glTranslate(pos);

    // glx::glActiveTextureARB(GL_TEXTURE0_ARB);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    for (i = 0; i < MaxCometTailPoints; i++)
    {
        float brightness = 1.0f - (float) i / (float) (MaxCometTailPoints - 1);
        Vec3f v0, v1;
        float sectionLength;
        if (i != 0 && i != MaxCometTailPoints - 1)
        {
            v0 = cometPoints[i] - cometPoints[i - 1];
            v1 = cometPoints[i + 1] - cometPoints[i];
            sectionLength = v0.length();
            v0.normalize();
            v1.normalize();
            Vec3f axis = v1 ^ v0;
            float axisLength = axis.length();
            if (axisLength > 1e-5f)
            {
                axis *= 1.0f / axisLength;
                q.setAxisAngle(axis, (float) asin(axisLength));
                Mat3f m = q.toMatrix3();

                u = u * m;
                v = v * m;
                w = w * m;
            }
        }
        else if (i == 0)
        {
            v0 = cometPoints[i + 1] - cometPoints[i];
            sectionLength = v0.length();
            v0.normalize();
            v1 = v0;
        }
        else
        {
            v0 = v1 = cometPoints[i] - cometPoints[i - 1];
            sectionLength = v0.length();
            v0.normalize();
            v1 = v0;
        }

        float radius = (float) i / (float) MaxCometTailPoints *
            dustTailRadius;
        float dr = (dustTailRadius / (float) MaxCometTailPoints) /
            sectionLength;
        float w0 = (float) atan(dr);
        float w1 = (float) sqrt(1.0f - square(w0));

        for (int j = 0; j < CometTailSlices; j++)
        {
            float theta = (float) (2 * PI * (float) j / CometTailSlices);
            float s = (float) sin(theta);
            float c = (float) cos(theta);
            CometTailVertex* vtx = &cometTailVertices[i * CometTailSlices + j];
            vtx->normal = u * (s * w1) + w * (c * w1) + v * w0;
            s *= radius;
            c *= radius;

            vtx->point = Point3f(cometPoints[i].x + u.x * s + w.x * c,
                                 cometPoints[i].y + u.y * s + w.y * c,
                                 cometPoints[i].z + u.z * s + w.z * c);
            vtx->brightness = brightness;
            vtx->paraboloidPoint =
                Point3f(c, s, square((float) i / (float) MaxCometTailPoints));
        }
    }

    Vec3f viewDir = pos - Point3f(0.0f, 0.0f, 0.0f);
    viewDir.normalize();

    glDisable(GL_CULL_FACE);
    for (i = 0; i < MaxCometTailPoints - 1; i++)
    {
        glBegin(GL_QUAD_STRIP);
        int n = i * CometTailSlices;
        for (int j = 0; j < CometTailSlices; j++)
        {
            ProcessCometTailVertex(cometTailVertices[n + j], viewDir);
            ProcessCometTailVertex(cometTailVertices[n + j + CometTailSlices],
                                   viewDir);
        }
        ProcessCometTailVertex(cometTailVertices[n], viewDir);
        ProcessCometTailVertex(cometTailVertices[n + CometTailSlices],
                               viewDir);
        glEnd();
    }
    glEnable(GL_CULL_FACE);

    glBegin(GL_LINE_STRIP);
    for (i = 0; i < MaxCometTailPoints; i++)
    {
        glVertex(cometPoints[i]);
    }
    glEnd();

    glDepthMask(GL_TRUE);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);

    glPopMatrix();
}


void Renderer::renderPlanetarySystem(const Star& sun,
                                     const PlanetarySystem& solSystem,
                                     const Observer& observer,
                                     double now,
                                     bool showLabels)
{
    Point3f starPos = sun.getPosition();
    Point3d observerPos = astro::heliocentricPosition(observer.getPosition(), starPos);

    int nBodies = solSystem.getSystemSize();
    for (int i = 0; i < nBodies; i++)
    {
        Body* body = solSystem.getBody(i);
        if (!body->extant(now))
            continue;

        Point3d localPos = body->getOrbit()->positionAtTime(now);
        Point3d bodyPos = body->getHeliocentricPosition(now);
        
        // We now have the positions of the observer and the planet relative
        // to the sun.  From these, compute the position of the planet
        // relative to the observer.
        Vec3d posd = bodyPos - observerPos;

        double distanceFromObserver = posd.length();
        float appMag = body->getApparentMagnitude(sun,
                                                  bodyPos - Point3d(0, 0, 0),
                                                  posd);
        Vec3f pos((float) posd.x, (float) posd.y, (float) posd.z);

        // Compute the size of the planet/moon disc in pixels
        float discSize = (body->getRadius() / (float) distanceFromObserver) / pixelSize;

        // if (discSize > 1 || appMag < 1.0f / brightnessScale)
        if ((discSize > 1 || appMag < faintestPlanetMag) &&
            body->getClassification() != Body::Invisible)
        {
            RenderListEntry rle;
            rle.body = body;
            rle.star = NULL;
            rle.isCometTail = false;
            rle.position = Point3f(pos.x, pos.y, pos.z);
            rle.sun = Vec3f((float) -bodyPos.x, (float) -bodyPos.y, (float) -bodyPos.z);
            rle.distance = (float) distanceFromObserver;
            rle.radius = body->getRadius();
            rle.discSizeInPixels = discSize;
            rle.appMag = appMag;
            renderList.insert(renderList.end(), rle);
        }

        if (body->getClassification() == Body::Comet &&
            (renderFlags & ShowCometTails) != 0)
        {
            float radius = 10000000.0f; // body->getRadius() * 1000000.0f;
            discSize = (radius / (float) distanceFromObserver) / pixelSize;
            if (discSize > 1)
            {
                RenderListEntry rle;
                rle.body = body;
                rle.star = NULL;
                rle.isCometTail = true;
                rle.position = Point3f(pos.x, pos.y, pos.z);
                rle.radius = radius;
                rle.sun = Vec3f((float) -bodyPos.x, (float) -bodyPos.y, (float) -bodyPos.z);
                rle.distance = (float) distanceFromObserver;
                rle.radius = radius;
                rle.discSizeInPixels = discSize;
                rle.appMag = appMag;
                renderList.insert(renderList.end(), rle);
            }
        }

        if (showLabels && (pos * conjugate(observer.getOrientation()).toMatrix3()).z < 0)
        {
            float boundingRadiusSize = (float) (body->getOrbit()->getBoundingRadius() / distanceFromObserver) / pixelSize;
            if (boundingRadiusSize > minOrbitSize)
            {
                Color labelColor;
                bool showLabel = false;

                switch (body->getClassification())
                {
                case Body::Planet:
                    if ((labelMode & PlanetLabels) != 0)
                    {
                        labelColor = Color(0.0f, 1.0f, 0.0f);
                        showLabel = true;
                    }
                    break;
                case Body::Moon:
                    if ((labelMode & MoonLabels) != 0)
                    {
                        labelColor = Color(0.0f, 0.65f, 0.0f);
                        showLabel = true;
                    }
                    break;
                case Body::Asteroid:
                    if ((labelMode & AsteroidLabels) != 0)
                    {
                        labelColor = Color(0.7f, 0.4f, 0.0f);
                        showLabel = true;
                    }
                    break;
                case Body::Comet:
                    if ((labelMode & CometLabels) != 0)
                    {
                        labelColor = Color(0.0f, 1.0f, 1.0f);
                        showLabel = true;
                    }
                    break;
                case Body::Spacecraft:
                    if ((labelMode & SpacecraftLabels) != 0)
                    {
                        labelColor = Color(0.6f, 0.6f, 0.6f);
                        showLabel = true;
                    }
                    break;
                }

                if (showLabel)
                {
                    addSortedLabel(body->getName(), labelColor,
                                   Point3f(pos.x, pos.y, pos.z));
                }
            }
        }

        if (appMag < faintestPlanetMag)
        {
            const PlanetarySystem* satellites = body->getSatellites();
            if (satellites != NULL)
            {
                renderPlanetarySystem(sun, *satellites, observer,
                                      now, showLabels);
            }
        }
    }
}


class StarRenderer : public StarHandler
{
public:
    StarRenderer();
    ~StarRenderer() {};
    
    void process(const Star&, float, float);

public:
    const Observer* observer;
    Point3f position;
    Vec3f viewNormal;

    vector<Renderer::Particle>* glareParticles;
    vector<RenderListEntry>* renderList;
    Renderer::StarVertexBuffer* starVertexBuffer;

    float faintestMagNight;
    float fov;
    float size;
    float pixelSize;
    float faintestMag;
    float saturationMag;
    float brightnessScale;
    float brightnessBias;
    float distanceLimit;

    int nRendered;
    int nClose;
    int nBright;
    int nProcessed;

    bool useDiscs;
    float maxDiscSize;
};

StarRenderer::StarRenderer() :
    starVertexBuffer(0),
    distanceLimit(1.0e6f),
    nRendered(0),
    nClose(0),
    nBright(0),
    nProcessed(0),
    useDiscs(false),
    maxDiscSize(1.0f)
{
}

void StarRenderer::process(const Star& star, float distance, float appMag)
{
    nProcessed++;

    Point3f starPos = star.getPosition();
    Vec3f relPos = starPos - position;

    if (distance > distanceLimit)
        return;

    if (relPos * viewNormal > 0 || relPos.x * relPos.x < 0.1f)
    {
        Color starColor = star.getStellarClass().getApparentColor();
        float renderDistance = distance;
        float s = renderDistance * size;
        float discSizeInPixels = 0.0f;

        // Special handling for stars less than one light year away . . .
        // We can't just go ahead and render a nearby star in the usual way
        // for two reasons:
        //   * It may be clipped by the near plane
        //   * It may be large enough that we should render it as a mesh
        //     instead of a particle
        // It's possible that the second condition might apply for stars
        // further than one light year away if the star is huge, the fov is
        // very small and the resolution is high.  We'll ignore this for now
        // and use the most inexpensive test possible . . .
        if (distance < 1.0f)
        {
            // Compute the position of the observer relative to the star.
            // This is a much more accurate (and expensive) distance
            // calculation than the previous one which used the observer's
            // position rounded off to floats.
            Point3d hPos = astro::heliocentricPosition(observer->getPosition(), starPos);
            relPos = Vec3f((float) hPos.x, (float) hPos.y, (float) hPos.z) *
                -astro::kilometersToLightYears(1.0f),
            distance = relPos.length();

            // Recompute apparent magnitude using new distance computation
            appMag = astro::absToAppMag(star.getAbsoluteMagnitude(),
                                        distance);

            float f = RenderDistance / distance;
            renderDistance = RenderDistance;
            starPos = position + relPos * f;

            float radius = star.getRadius();
            discSizeInPixels = radius / astro::lightYearsToKilometers(distance) /pixelSize;
            nClose++;
        }

        if (discSizeInPixels <= 1)
        {
            float alpha = (faintestMag - appMag) * brightnessScale + brightnessBias;

            if (useDiscs)
            {
                float discSize = size;
                if (alpha < 0.0f)
                {
                    alpha = 0.0f;
                }
                else if (alpha > 1.0f)
                {
                    discSize = min(discSize * (2.0f * alpha - 1.0f), maxDiscSize);
                    alpha = 1.0f;
                }
                starVertexBuffer->addStar(starPos,
                                          Color(starColor, alpha),
                                          renderDistance * discSize);
            }
            else
            {
                alpha = clamp(alpha);
                starVertexBuffer->addStar(starPos,
                                          Color(starColor, alpha),
                                          renderDistance * size);
            }

            nRendered++;

            // If the star is brighter than the saturation magnitude, add a
            // halo around it to make it appear more brilliant.  This is a
            // hack to compensate for the limited dynamic range of monitors.
            if (appMag < saturationMag)
            {
                Renderer::Particle p;
                p.center = starPos;
                p.size = renderDistance * size;
                p.color = Color(starColor, alpha);

                alpha = 0.4f * clamp((appMag - saturationMag) * -0.8f);
                s = renderDistance * 0.001f * (3 - (appMag - saturationMag)) * 2;
                if (s > p.size * 3 )
		    p.size = s * 2.0f/(1.0f +FOV/fov);
                else
                    p.size = p.size * 3;
                p.color = Color(starColor, alpha);
                glareParticles->insert(glareParticles->end(), p);
                nBright++;
            }
        }
        else
        {
            RenderListEntry rle;
            rle.star = &star;
            rle.body = NULL;
            rle.isCometTail = false;
            
            // Objects in the render list are always rendered relative to
            // a viewer at the origin--this is different than for distant
            // stars.
            float scale = astro::lightYearsToKilometers(1.0f);
            rle.position = Point3f(relPos.x * scale, relPos.y * scale, relPos.z * scale);
            rle.distance = rle.position.distanceFromOrigin();
            rle.radius = star.getRadius();
            rle.discSizeInPixels = discSizeInPixels;
            rle.appMag = appMag;
            renderList->insert(renderList->end(), rle);
        }
    }
}


void Renderer::renderStars(const StarDatabase& starDB,
                           float faintestMagNight,
                           const Observer& observer)
{
    StarRenderer starRenderer;
    Point3f observerPos = (Point3f) observer.getPosition();
    observerPos.x *= 1e-6f;
    observerPos.y *= 1e-6f;
    observerPos.z *= 1e-6f;

    starRenderer.observer = &observer;
    starRenderer.position = observerPos;
    starRenderer.viewNormal = Vec3f(0, 0, -1) * observer.getOrientation().toMatrix3();
    starRenderer.glareParticles = &glareParticles;
    starRenderer.renderList = &renderList;
    starRenderer.starVertexBuffer = starVertexBuffer;
    starRenderer.faintestMagNight = faintestMagNight;
    starRenderer.fov              = fov;
    // size/pixelSize =0.86 at 120deg, 1.43 at 45deg and 1.6 at 0deg.
    starRenderer.size = pixelSize * 1.6f / corrFac;
    starRenderer.pixelSize = pixelSize;
    starRenderer.brightnessScale = brightnessScale * corrFac;
    starRenderer.brightnessBias = brightnessBias;
    starRenderer.faintestMag = faintestMag;
    starRenderer.saturationMag = saturationMag;
    starRenderer.distanceLimit = distanceLimit;
    if (starStyle == ScaledDiscStars)
    {
        starRenderer.useDiscs = true;
        starRenderer.brightnessScale *= 2.0f;
        starRenderer.maxDiscSize = starRenderer.size * MaxScaledDiscStarSize;
    }

    glareParticles.clear();

    starVertexBuffer->setBillboardOrientation(observer.getOrientation());

    glEnable(GL_TEXTURE_2D);

    starTex->bind();
    starRenderer.starVertexBuffer->start(starStyle == PointStars);
    starDB.findVisibleStars(starRenderer,
                            observerPos,
                            observer.getOrientation(),
                            degToRad(fov),
                            (float) windowWidth / (float) windowHeight,
                            faintestMagNight);
    starRenderer.starVertexBuffer->finish();

    glareTex->bind();
    renderParticles(glareParticles, observer.getOrientation());
}


void Renderer::renderDeepSkyObjects(const DeepSkyCatalog& catalog,
                                    const Observer& observer)
{
    Point3d observerPos = (Point3d) observer.getPosition();
    observerPos.x *= 1e-6;
    observerPos.y *= 1e-6;
    observerPos.z *= 1e-6;

    Frustum frustum(degToRad(fov),
		    (float) windowWidth / (float) windowHeight,
		    MinNearPlaneDistance);

    Mat3f viewMat = observer.getOrientation().toMatrix3();
    Vec3f v0 = Vec3f(-1, -1, 0) * viewMat;
    Vec3f v1 = Vec3f( 1, -1, 0) * viewMat;
    Vec3f v2 = Vec3f( 1,  1, 0) * viewMat;
    Vec3f v3 = Vec3f(-1,  1, 0) * viewMat;

    // Kludgy way to diminish brightness of galaxies based on faintest
    // magnitude.  I need to rethink how galaxies are rendered.
    float brightness = min((faintestMag - 2.0f) / 4.0f, 1.0f);
    if (brightness < 0.0f)
        return;

    // Render any line primitives with smooth lines (mostly to make
    // graticules look good.)
    if ((renderFlags & ShowSmoothLines) != 0)
        enableSmoothLines();

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (DeepSkyCatalog::const_iterator iter = catalog.begin();
         iter != catalog.end(); iter++)
    {
        DeepSkyObject* obj = *iter;
        Point3d pos = obj->getPosition();
        float radius = obj->getRadius();
        Vec3f offset = Vec3f((float) (pos.x - observerPos.x),
                             (float) (pos.y - observerPos.y),
                             (float) (pos.z - observerPos.z));
        Point3f center = Point3f(0.0f, 0.0f, 0.0f) + offset *
            conjugate(observer.getOrientation()).toMatrix3(); 

        // Test the object's bounding sphere against the view frustum
	if (frustum.testSphere(center, radius) != Frustum::Outside)
        {
            float distanceToObject = offset.length() - radius;
            if (distanceToObject < 0)
                distanceToObject = 0;
            float minimumFeatureSize = pixelSize * 0.5f * distanceToObject;

            if (radius < 10.0)
            {
                // Small objects may be prone to clipping; give them special
                // handling.  We don't want to always set the projection
                // matrix, since that could be expensive with large galaxy
                // catalogs.
                float nearZ = distanceToObject;
                float farZ = offset.length() + radius * 2;
                if (nearZ < radius * 0.001f)
                {
                    nearZ = radius * 0.001f;
                    farZ = nearZ * 10000.0f;
                }

                glMatrixMode(GL_PROJECTION);
                glPushMatrix();
                glLoadIdentity();
                gluPerspective(fov,
                               (float) windowWidth / (float) windowHeight,
                               nearZ, farZ);
                glMatrixMode(GL_MODELVIEW);
            }

            glPushMatrix();
            glTranslate(offset);
            obj->render(offset,
                        observer.getOrientation(),
                        brightness,
                        pixelSize);
            glPopMatrix();

            if (radius < 10.0)
            {
                glMatrixMode(GL_PROJECTION);
                glPopMatrix();
                glMatrixMode(GL_MODELVIEW);
            }
        }
    }

    if ((renderFlags & ShowSmoothLines) != 0)
        disableSmoothLines();
}


void Renderer::renderCelestialSphere(const Observer& observer)
{
    int raDivisions = 12;
    int decDivisions = 12;
    int nSections = 60;
    float radius = 10.0f;

    int i;
    for (i = 0; i < raDivisions; i++)
    {
        float ra = (float) i / (float) raDivisions * 24.0f;

        glBegin(GL_LINE_STRIP);
        for (int j = 0; j <= nSections; j++)
        {
            float dec = ((float) j / (float) nSections) * 180 - 90;
            glVertex(astro::equatorialToCelestialCart(ra, dec, radius));
        }
        glEnd();
    }

    for (i = 1; i < decDivisions; i++)
    {
        float dec = (float) i / (float) decDivisions * 180 - 90;
        glBegin(GL_LINE_LOOP);
        for (int j = 0; j < nSections; j++)
        {
            float ra = (float) j / (float) nSections * 24.0f;
            glVertex(astro::equatorialToCelestialCart(ra, dec, radius));
        }
        glEnd();
    }

    for (i = 0; i < nCoordLabels; i++)
    {
        Point3f pos = astro::equatorialToCelestialCart(coordLabels[i].ra,
                                                       coordLabels[i].dec,
                                                       radius);
        if ((pos * conjugate(observer.getOrientation()).toMatrix3()).z < 0)
        {
            addLabel(coordLabels[i].label, Color(0.3f, 0.7f, 0.7f, 0.85f), pos);
        }
    }
}


void Renderer::labelGalaxies(const DeepSkyCatalog& catalog,
                             const Observer& observer)
{
    Point3f observerPos_ly = (Point3f) observer.getPosition() * ((float)1e-6);

    for (DeepSkyCatalog::const_iterator iter = catalog.begin();
         iter != catalog.end(); iter++)
    {
        DeepSkyObject* obj = *iter;
        Point3d posd = obj->getPosition();
        Point3f pos((float) posd.x, (float) posd.y, (float) posd.z);

        Vec3f rpos = pos - observerPos_ly;
        if ((rpos * conjugate(observer.getOrientation()).toMatrix3()).z < 0)
        {
            addLabel(obj->getName(), Color(0.7f, 0.7f, 0.0f),
                     Point3f(rpos.x, rpos.y, rpos.z));
        }
    }
}


void Renderer::labelStars(const vector<StarLabel>& stars,
                          const StarDatabase& starDB,
                          const Observer& observer)
{
    Point3f observerPos_ly = (Point3f) observer.getPosition() * ((float)1e-6);

    for (vector<StarLabel>::const_iterator iter = stars.begin(); iter != stars.end(); iter++)
    {
        Star* star = iter->star;
        Point3f pos = star->getPosition();
        float distance = pos.distanceTo(observerPos_ly);
        float appMag = (distance > 0.0f) ?
            astro::absToAppMag(star->getAbsoluteMagnitude(), distance) : -100.0f;
        
        if (appMag < faintestMag && distance <= distanceLimit)
        {
            Vec3f rpos = pos - observerPos_ly;

            // Use a more accurate and expensive calculation if the
            // distance to the star is less than a light year.  Single
            // precision arithmetic isn't good enough when we're very close
            // to the star.
            if (distance < 1.0f)
            {
                Point3d hpos = astro::heliocentricPosition(observer.getPosition(), pos);
                rpos = Vec3f((float) -hpos.x, (float) -hpos.y, (float) -hpos.z);
            }

            if ((rpos * conjugate(observer.getOrientation()).toMatrix3()).z < 0)
            {
                addLabel(iter->label,
                         Color(0.3f, 0.3f, 1.0f),
                         Point3f(rpos.x, rpos.y, rpos.z));
            }
        }
    }
}


void Renderer::labelConstellations(const AsterismList& asterisms,
                                   const Observer& observer)
{
    Point3f observerPos = (Point3f) observer.getPosition();

    for (AsterismList::const_iterator iter = asterisms.begin();
         iter != asterisms.end(); iter++)
    {
        Asterism* ast = *iter;
        if (ast->getChainCount() > 0)
        {
            const Asterism::Chain& chain = ast->getChain(0);

            if (chain.size() > 0)
            {
                // The constellation label is positioned at the average
                // position of all stars in the first chain.  This usually
                // gives reasonable results.
                Vec3f avg(0, 0, 0);
                for (Asterism::Chain::const_iterator iter = chain.begin();
                     iter != chain.end(); iter++)
                    avg += (*iter - Point3f(0, 0, 0));

                avg = avg / (float) chain.size();
                avg = avg * 1e6f;
                Vec3f rpos = Point3f(avg.x, avg.y, avg.z) - observerPos;
                if ((rpos * conjugate(observer.getOrientation()).toMatrix3()).z < 0) {
                    addLabel(ast->getName(),
                             Color(0.5f, 0.0f, 1.0f, 1.0f),
                             Point3f(rpos.x, rpos.y, rpos.z));
                }
            }
        }
    }
}


void Renderer::renderParticles(const vector<Particle>& particles,
                               Quatf orientation)
{
    int nParticles = particles.size();

    Mat3f m = orientation.toMatrix3();
    Vec3f v0 = Vec3f(-1, -1, 0) * m;
    Vec3f v1 = Vec3f( 1, -1, 0) * m;
    Vec3f v2 = Vec3f( 1,  1, 0) * m;
    Vec3f v3 = Vec3f(-1,  1, 0) * m;

    glBegin(GL_QUADS);
    for (int i = 0; i < nParticles; i++)
    {
        Point3f center = particles[i].center;
        float size = particles[i].size;

        glColor(particles[i].color);
        glTexCoord2f(0, 0);
        glVertex(center + (v0 * size));
        glTexCoord2f(1, 0);
        glVertex(center + (v1 * size));
        glTexCoord2f(1, 1);
        glVertex(center + (v2 * size));
        glTexCoord2f(0, 1);
        glVertex(center + (v3 * size));
    }
    glEnd();
}


void Renderer::renderLabels()
{
    if (font == NULL)
        return;

    //glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    font->bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, windowWidth, 0, windowHeight);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glTranslatef(GLfloat((int) (windowWidth / 2)),
                 GLfloat((int) (windowHeight / 2)), 0);

    for (int i = 0; i < (int) labels.size(); i++)
    {
        glColor(labels[i].color);
        glPushMatrix();
        glTranslatef((int) labels[i].position.x + PixelOffset + 2.0f,
                     (int) labels[i].position.y + PixelOffset,
                     0.0f);
        font->render(labels[i].text);
        glPopMatrix();
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_DEPTH_TEST);
}


vector<Renderer::Label>::iterator
Renderer::renderSortedLabels(vector<Label>::iterator iter, float depth)
{
    if (font == NULL)
        return iter;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    font->bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, windowWidth, 0, windowHeight);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glTranslatef(GLfloat((int) (windowWidth / 2)),
                 GLfloat((int) (windowHeight / 2)), 0);

    for (; iter != depthSortedLabels.end() && iter->position.z > depth; iter++)
    {
        glColor(iter->color);
        glPushMatrix();
        glTranslatef((int) iter->position.x + PixelOffset + 2.0f,
                     (int) iter->position.y + PixelOffset,
                     0.0f);
        font->render(iter->text);
        glPopMatrix();
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_DEPTH_TEST);

    return iter;
}


void Renderer::renderMarkers(const MarkerList& markers,
                             const UniversalCoord& position,
                             const Quatf& orientation,
                             double jd)
{
    double identity4x4[16] = { 1.0, 0.0, 0.0, 0.0,
                               0.0, 1.0, 0.0, 0.0,
                               0.0, 0.0, 1.0, 0.0,
                               0.0, 0.0, 0.0, 1.0
                             };
    int view[4] = { 0, 0, 0, 0 };
    view[0] = -windowWidth / 2;
    view[1] = -windowHeight / 2;
    view[2] = windowWidth;
    view[3] = windowHeight;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, windowWidth, 0, windowHeight);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glTranslatef(GLfloat((int) (windowWidth / 2)),
                 GLfloat((int) (windowHeight / 2)), 0);

    Mat3f rot = conjugate(orientation).toMatrix3();

    for (MarkerList::const_iterator iter = markers.begin();
         iter != markers.end(); iter++)
    {
        UniversalCoord uc = iter->getPosition(jd);
        Vec3d offset = uc - position;
        Vec3f eyepos = Vec3f((float) offset.x, (float) offset.y, (float) offset.z) * rot;
        eyepos.normalize();
        eyepos *= 1000.0f;

        double winX, winY, winZ;
        if (gluProject(eyepos.x, eyepos.y, eyepos.z,
                       identity4x4,
                       projMatrix,
                       (const GLint*) view,
                       &winX, &winY, &winZ) != GL_FALSE)
        {
            if (eyepos.z < 0.0f)
            {
                glPushMatrix();
                glTranslatef((GLfloat) winX, (GLfloat) winY, 0.0f);

                glColor(iter->getColor());
                iter->render();

                glPopMatrix();
            }
        }
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_DEPTH_TEST);
}


float Renderer::getSaturationMagnitude() const
{
    return saturationMag;
}


void Renderer::setSaturationMagnitude(float mag)
{
    saturationMag = mag;
}


float Renderer::getBrightnessBias() const
{
    return brightnessBias;
}


void Renderer::setBrightnessBias(float bias)
{
    brightnessBias = bias;
}


void Renderer::setStarStyle(StarStyle style)
{
    starStyle = style;
}


Renderer::StarStyle Renderer::getStarStyle() const
{
    return starStyle;
}


Renderer::StarVertexBuffer::StarVertexBuffer(unsigned int _capacity) :
    capacity(_capacity),
    vertices(NULL),
    texCoords(NULL),
    colors(NULL),
    usePoints(false)
{
    nStars = 0;
    vertices = new float[capacity * 12];
    texCoords = new float[capacity * 8];
    colors = new unsigned char[capacity * 16];

    // Fill the texture coordinate array now, since it will always have
    // the same contents.
    for (unsigned int i = 0; i < capacity; i++)
    {
        unsigned int n = i * 8;
        texCoords[n    ] = 0; texCoords[n + 1] = 0;
        texCoords[n + 2] = 1; texCoords[n + 3] = 0;
        texCoords[n + 4] = 1; texCoords[n + 5] = 1;
        texCoords[n + 6] = 0; texCoords[n + 7] = 1;
    }
}

Renderer::StarVertexBuffer::~StarVertexBuffer()
{
    if (vertices != NULL)
        delete vertices;
    if (colors != NULL)
        delete colors;
    if (texCoords != NULL)
        delete texCoords;
}

void Renderer::StarVertexBuffer::start(bool _usePoints)
{
    usePoints = _usePoints;

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors);
    if (!usePoints)
    {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
    }
    else
    {
        // An option to control the size of the stars would be helpful.
        // Which size looks best depends a lot on the resolution and the
        // type of display device.
        // glPointSize(2.0f);
        // glEnable(GL_POINT_SMOOTH);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_TEXTURE_2D);
    }
    glDisableClientState(GL_NORMAL_ARRAY);
}

void Renderer::StarVertexBuffer::render()
{
    if (nStars != 0)
    {
        if (usePoints)
            glDrawArrays(GL_POINTS, 0, nStars);
        else
            glDrawArrays(GL_QUADS, 0, nStars * 4);
        nStars = 0;
    }
}

void Renderer::StarVertexBuffer::finish()
{
    render();
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (usePoints)
        glEnable(GL_TEXTURE_2D);
}

void Renderer::StarVertexBuffer::addStar(const Point3f& pos,
                                         const Color& color,
                                         float size)
{
    if (nStars < capacity)
    {
        if (usePoints)
        {
            int n = nStars * 3;
            vertices[n + 0] = pos.x;
            vertices[n + 1] = pos.y;
            vertices[n + 2] = pos.z;
            color.get(colors + nStars * 4);
        }
        else
        {
            int n = nStars * 12;
            vertices[n + 0]  = pos.x + v0.x * size;
            vertices[n + 1]  = pos.y + v0.y * size;
            vertices[n + 2]  = pos.z + v0.z * size;
            vertices[n + 3]  = pos.x + v1.x * size;
            vertices[n + 4]  = pos.y + v1.y * size;
            vertices[n + 5]  = pos.z + v1.z * size;
            vertices[n + 6]  = pos.x + v2.x * size;
            vertices[n + 7]  = pos.y + v2.y * size;
            vertices[n + 8]  = pos.z + v2.z * size;
            vertices[n + 9]  = pos.x + v3.x * size;
            vertices[n + 10] = pos.y + v3.y * size;
            vertices[n + 11] = pos.z + v3.z * size;
            n = nStars * 16;
            color.get(colors + n);
            color.get(colors + n + 4);
            color.get(colors + n + 8);
            color.get(colors + n + 12);
        }
        nStars++;
    }

    if (nStars == capacity)
    {
        render();
        nStars = 0;
    }
}

void Renderer::StarVertexBuffer::setBillboardOrientation(const Quatf& q)
{
    Mat3f m = q.toMatrix3();
    v0 = Vec3f(-1, -1, 0) * m;
    v1 = Vec3f( 1, -1, 0) * m;
    v2 = Vec3f( 1,  1, 0) * m;
    v3 = Vec3f(-1,  1, 0) * m;
}
                               

void Renderer::loadTextures(Body* body)
{
    Surface& surface = body->getSurface();

    if (surface.baseTexture.tex[textureResolution] != InvalidResource)
        surface.baseTexture.find(textureResolution);
    if ((surface.appearanceFlags & Surface::ApplyBumpMap) != 0 &&
        context->bumpMappingSupported() &&
        surface.bumpTexture.tex[textureResolution] != InvalidResource)
        surface.bumpTexture.find(textureResolution);
    if ((surface.appearanceFlags & Surface::ApplyNightMap) != 0 &&
        (renderFlags & ShowNightMaps) != 0)
        surface.nightTexture.find(textureResolution);
    if ((surface.appearanceFlags & Surface::SeparateSpecularMap) != 0 &&
        surface.specularTexture.tex[textureResolution] != InvalidResource)
        surface.specularTexture.find(textureResolution);

    if ((renderFlags & ShowCloudMaps) != 0 &&
        body->getAtmosphere() != NULL &&
        body->getAtmosphere()->cloudTexture.tex[textureResolution] != InvalidResource)
    {
        body->getAtmosphere()->cloudTexture.find(textureResolution);
    }

    if (body->getRings() != NULL &&
        body->getRings()->texture.tex[textureResolution] != InvalidResource)
    {
        body->getRings()->texture.find(textureResolution);
    }
}
