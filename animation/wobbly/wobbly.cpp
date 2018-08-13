/*
 * animation/wobbly/wobbly.cpp
 *
 * Copyright 2018 Endless Mobile, Inc.
 *
 * libanimation is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * libanimation is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with eos-companion-app-service.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * A spring model for implementing "wobbly" textures.
 *
 * This works by subdividing a texture's unit-coordinate-space
 * into a series of points, called "Objects". Object in turn, are
 * linked together into a mesh by springs, with each object being
 * connected bidirectionally by a spring both south and east of it,
 * for instance:
 *
 *    o(1)---s(1)---o(2)
 *    |               |
 *    |               |
 *    s(2)          s(4)
 *    |               |
 *    |               |
 *    o(3)---s(3)---o(4)
 *
 * The force delta applied to each spring at time t is
 *
 *     dF[t] = (distance * (o[a] - o[b])) * k
 *
 * This is in turn applied to the child objects
 *
 *     F[a] += -dF[t]
 *     F[b] += dF[t]
 *
 * Velocity at for a given time (t) is calculated by:
 *
 *     A[t] = (F/M)
 *     dV[t] = A[t] * dT
 *     V[t] += dV[t]
 *
 * We don't want infinite acceleration so we apply a friction vector
 * proportional to the velocity of the point at time (t)
 *
 *     Fr[t] = V[t] * Fk
 *
 * "Anchor" objects are special. No force can ever be applied to them,
 * and the feedback effect of this is that greater force will be
 * applied to its siblings.
 *
 * See LICENCE.md for Copyright information.
 *
 * The original implementation of this for Luminocity is
 * Copyright (c) Kristian Hoegsberg
 *
 * The original implementation of this for Compiz is
 * Copyright (c) 2005 David Reveman
 */

#include <math.h>                       // for ceil, round
#include <cstddef>                      // for size_t
#include <cassert>                      // for assert

#include <algorithm>                    // for copy, min, fill_n, max
#include <array>                        // for array, array<>::iterator, etc
#include <functional>                   // for __bind, __base, bind, etc
#include <limits>                       // for numeric_limits
#include <memory>                       // for unique_ptr, etc
#include <stdexcept>                    // for runtime_error
#include <tuple>                        // for get, make_tuple
#include <type_traits>                  // for move, etc
#include <vector>                       // for vector

#include <experimental/optional>        // for optional

#include <animation/wobbly/geometry.h>  // for PointView, PointModel, etc
#include <animation/wobbly/geometry_traits.h> // for assign, scale, etc
#include <animation/wobbly/wobbly.h>              // for Model, etc

#include "wobbly_internal.h"            // for Spring, MeshArray, etc

namespace wg = ::wobbly::geometry;
namespace wgd = ::wobbly::geometry::dimension;

void
wobbly::Anchor::MovableAnchorDeleter::operator () (MovableAnchor *anchor)
{
    delete anchor;
}

wobbly::Anchor::Anchor ()
{
}

wobbly::Anchor::~Anchor ()
{
}

void
wobbly::Anchor::MoveBy (wobbly::Vector const &delta) noexcept
{
    priv->MoveBy (delta);
}

wobbly::Anchor
wobbly::Anchor::Create (Impl &&impl)
{
    struct ConstructibleAnchor :
        public wobbly::Anchor
    {
        ConstructibleAnchor (Impl &&impl)
        {
            priv = std::move (impl);
        }
    };

    return ConstructibleAnchor (std::move (impl));
}

namespace
{
    size_t
    SpringCountForGridSize (unsigned int width,
                            unsigned int height)
    {
        return ((width - 1) * (height - 1)) * 2 + width + height - 2;
    }
}

namespace
{
    size_t NextSpringID ()
    {
        static size_t CurrentSpringID = 0;
        return ++CurrentSpringID;
    }
}

wobbly::Spring::Spring (PointView <double>       &&forceA,
                        PointView <double>       &&forceB,
                        PointView <double const> &&posA,
                        PointView <double const> &&posB,
                        Vector                   distance,
                        IDFetchStrategy    const &fetchID) :
    forceA (std::move (forceA)),
    forceB (std::move (forceB)),
    posA (std::move (posA)),
    posB (std::move (posB)),
    desiredDistance (distance),
    id (fetchID ())
{
}

/* cppcheck doesn't understand delegating constructors yet */
// cppcheck-suppress uninitMemberVar
wobbly::Spring::Spring (PointView <double>       &&forceA,
                        PointView <double>       &&forceB,
                        PointView <double const> &&posA,
                        PointView <double const> &&posB,
                        Vector             const &distance) :
    Spring (std::move (forceA),
            std::move (forceB),
            std::move (posA),
            std::move (posB),
            distance,
            std::bind (NextSpringID))
{
}

wobbly::Spring::ConstructionPackage
wobbly::Spring::CreateWithTrackingID (PointView <double>       &&forceA,
                                      PointView <double>       &&forceB,
                                      PointView <const double> &&posA,
                                      PointView <const double> &&posB,
                                      Vector            const &distance)
{
    size_t trackingID (NextSpringID ());
    Spring spring (std::move (forceA),
                   std::move (forceB),
                   std::move (posA),
                   std::move (posB),
                   distance,
                   [&trackingID]() {
                       return trackingID;
                   });

    return {
               std::move (spring),
               ID (trackingID)
           };
}

wobbly::Spring::Spring (Spring &&spring) noexcept :
    forceA (std::move (spring.forceA)),
    forceB (std::move (spring.forceB)),
    posA (std::move (spring.posA)),
    posB (std::move (spring.posB)),
    desiredDistance (std::move (spring.desiredDistance)),
    id (std::move (spring.id))
{
}

wobbly::Spring &
wobbly::Spring::operator= (wobbly::Spring &&other) noexcept (true)
{
    if (this == &other)
        return *this;

    forceA = std::move (other.forceA);
    forceB = std::move (other.forceB);
    posA = std::move (other.posA);
    posB = std::move (other.posB);
    desiredDistance = std::move (other.desiredDistance);
    id = std::move (other.id);

    return *this;
}

wobbly::Spring::~Spring ()
{
}

void
wobbly::Spring::ScaleLength (Vector scaleFactor)
{
    wgd::pointwise_scale (desiredDistance, scaleFactor);
}

namespace
{
    std::vector <wobbly::Spring>
    GenerateBaseSpringMesh (wobbly::MeshArray    &points,
                            wobbly::MeshArray    &forces,
                            wobbly::Vector const &springDimensions)
    {
        using namespace wobbly;
        std::vector <Spring> springs;

        double const springWidth = wgd::get <0> (springDimensions);
        double const springHeight = wgd::get <1> (springDimensions);

        size_t const nSprings = SpringCountForGridSize (config::Width,
                                                        config::Height);

        springs.reserve (nSprings);

        for (size_t j = 0; j < config::Height; ++j)
        {
            for (size_t i = 0; i < config::Width; ++i)
            {
                typedef PointView <double> DPV;
                typedef PointView <double const> CDPV;

                size_t current = j * config::Width + i;
                size_t below = (j + 1) * config::Width + i;
                size_t right = j * config::Width + i + 1;

                /* Spring from us to object below us */
                if (j < config::Height - 1)
                {
                    springs.emplace_back (DPV (forces, current),
                                          DPV (forces, below),
                                          CDPV (points, current),
                                          CDPV (points, below),
                                          Vector (0.0, springHeight));
                }

                /* Spring from us to object right of us */
                if (i < config::Width - 1)
                {
                    springs.emplace_back (DPV (forces, current),
                                          DPV (forces, right),
                                          CDPV (points, current),
                                          CDPV (points, right),
                                          Vector (springWidth, 0.0f));
                }
            }
        }

        assert (springs.size () == nSprings);

        return springs;
    }
}

wobbly::SpringMesh::SpringMesh (MeshArray    &points,
                                Vector const &springDimensions) :
    mSprings (GenerateBaseSpringMesh (points, mForces, springDimensions)),
    mInserted ()
{
}

void
wobbly::SpringMesh::Scale (Vector const &scaleFactor)
{
    mSprings.Each ([&scaleFactor](Spring &spring) {
        spring.ScaleLength (scaleFactor);
    });
}

namespace
{
    wobbly::Spring const &
    FindSpringToSplit (wobbly::Point                    const &install,
                       wobbly::SpringMesh::SpringVector const &vector)
    {
        using namespace wobbly;

        wobbly::Spring const *found = nullptr;
        double primaryDistance = std::numeric_limits <double>::max ();
        double secondaryDistance = std::numeric_limits <double>::max ();

        vector.Each ([&](Spring const &spring) {
            double currentSpringPrimaryDistance =
                wgd::distance (spring.FirstPosition (), install);
            if (currentSpringPrimaryDistance < primaryDistance)
            {
                primaryDistance = currentSpringPrimaryDistance;
                secondaryDistance = std::numeric_limits <double>::max ();
                found = &spring;
            }

            /* Branch will be taken for any match of spring with
             * best primary distance */
            if (currentSpringPrimaryDistance == primaryDistance)
            {
                double currentSpringSecondaryDistance =
                    wgd::distance (spring.SecondPosition (), install);
                if (currentSpringSecondaryDistance < secondaryDistance)
                {
                    found = &spring;

                    /* This variable is used again on the next iteration
                     * of the loop, but cppcheck can't see through the
                     * implicit reference capture */
                    // cppcheck-suppress unreadVariable
                    secondaryDistance = currentSpringSecondaryDistance;
                }
            }
        });

        assert (found != nullptr);
        return *found;
    }
}

wobbly::SpringMesh::InstallResult
wobbly::SpringMesh::InstallAnchorSprings (Point         const &install,
                                          PosPreference const &firstPref,
                                          PosPreference const &secondPref)
{
    Spring const &found (FindSpringToSplit (install, mSprings));

    std::unique_ptr <double[]> data (new double[4]);
    std::fill_n (data.get (), 4, 0);
    wobbly::PointView <double> anchorView (data.get (), 0);
    wgd::assign (anchorView, install);

    /* We always want the spring to *read* the first and second
     * positions, although the positions for the purpose of
     * determining the desired distance may be different */
    PointView <double const> firstPoint (found.FirstPosition ());
    PointView <double const> secondPoint (found.SecondPosition ());
    PointView <double> firstForce (found.FirstForce ());
    PointView <double> secondForce (found.SecondForce ());

    /* These two points represent an absolute position, which, when
     * ths anchor position is subtracted, are the desired delta. */
    PointView <double const> firstDesired (firstPref (found));
    PointView <double const> secondDesired (secondPref (found));

    auto const insertSpring =
        [this, &data](wobbly::PointView <double const> meshPoint,
                      wobbly::PointView <double const> desiredPoint,
                      wobbly::PointView <double>       meshForce) {
            PointView <double const> anchorPoint (data.get (), 0);
            PointView <double> updatable (data.get (), 0);
            PointView <double> anchorForce (data.get (), 1);

            Vector delta;
            wgd::assign (delta, desiredPoint);
            wgd::pointwise_subtract (delta, anchorPoint);

            return mSprings.EmplaceAndTrack (std::move (anchorForce),
                                             std::move (meshForce),
                                             std::move (anchorPoint),
                                             std::move (meshPoint),
                                             delta);
        };

    /* Move out the split spring first */
    auto stolen =
        mSprings.TakeMatching ([&found](Spring const &spring) {
            return &spring == &found;
        });

    /* After this point, "found" is invalidated */
    auto first (insertSpring (std::move (firstPoint),
                              std::move (firstDesired),
                              std::move (firstForce)));
    auto second (insertSpring (std::move (secondPoint),
                               std::move (secondDesired),
                               std::move (secondForce)));

    /* We allow AnchorDataVector to have a temporary reference to a view
     * to our data for as long as our data lives so that we can move this
     * bit of data when the entire model moves* */
    auto trackedUpdatablePoint =
        mInserted.EmplaceAndTrack (std::move (anchorView));

    return {
               std::move (stolen),
               std::move (first),
               std::move (second),
               std::move (data),
               std::move (trackedUpdatablePoint)
           };
}

namespace wobbly
{
    class Model::Private
    {
        public:

            Private (Point    const &initialPosition,
                     double         width,
                     double         height,
                     Settings const &settings);

            std::array <wobbly::Point, 4> const
            Extremes () const;

            template <typename... Args>
            wobbly::Point
            TargetPositionByFullIntegration (Args&& ...additionalSteps) const;

            wobbly::Point
            TargetPosition () const;

            wobbly::Vector
            TileSize () const;

            double mWidth, mHeight;

            /* Anchor - is the point locked or unlocked */
            AnchorArray                   mAnchors;

            /* Estimated target positions, updated by anchors */
            TargetMesh                    mTargets;

            /* Constrainment data for each point */
            ConstrainmentStep             mConstrainment;

            /* Position of the point on the grid */
            BezierMesh                    mPositions;

            /* Force of each point on the grid */
            SpringStep <EulerIntegration> mSpring;

            /* Velocity of the point on the grid */
            EulerIntegration              mVelocityIntegrator;

            Model::Settings        const &mSettings;

            bool mCurrentlyUnequal;
    };
}

wobbly::Model::Private::Private (Point    const &initialPosition,
                                 double         width,
                                 double         height,
                                 Settings const &settings) :
    mWidth (width),
    mHeight (height),
    mTargets ([this](MeshArray &mesh) {
                  /* Use a full integration to compute the target position
                   * in case the anchor count ever drops to 1 - we don't
                   * want to short-circuit our own computation by just
                   * returning the existing targets array.
                   *
                   * Note that we do not pass the constrainment step here -
                   * the constrainment step uses the targets, which we're
                   * trying to compute. */
                  auto target (TargetPositionByFullIntegration ());
                  mesh::CalculatePositionArray (target,
                                                mesh,
                                                TileSize ());
              }),
    mConstrainment (settings.maximumRange, mTargets),
    mSpring (mVelocityIntegrator,
             mPositions.PointArray (),
             settings.springConstant,
             settings.friction,
             TileSize ()),
    mSettings (settings),
    mCurrentlyUnequal (false)
{
    /* First construct the position array */
    mesh::CalculatePositionArray (initialPosition,
                                  mPositions.PointArray (),
                                  TileSize ());

    /* Copy that into the mesh estimation */
    std::copy (mPositions.PointArray ().begin (),
               mPositions.PointArray ().end (),
               mTargets.PointArray ().begin ());
}

wobbly::Model::Settings wobbly::Model::DefaultSettings =
{
    wobbly::Model::DefaultSpringConstant,
    wobbly::Model::Friction,
    wobbly::Model::DefaultObjectRange
};

wobbly::Model::Model (Point const &initialPosition,
                      double      width,
                      double      height,
                      Settings    const &settings) :
    priv (new Private (initialPosition, width, height, settings))
{
}

wobbly::Model::Model (Point const &initialPosition,
                      double      width,
                      double      height) :
    priv (new Private (initialPosition, width, height, DefaultSettings))
{
}


wobbly::Model::~Model ()
{
}

namespace
{
    template <typename Integrator>
    bool PerformIntegration (wobbly::MeshArray         &positions,
                             wobbly::AnchorArray const &anchors,
                             Integrator                &&integrator)
    {
        return integrator (positions, anchors);
    }

    template <typename Integrator, typename... Remaining>
    bool PerformIntegration (wobbly::MeshArray         &positions,
                             wobbly::AnchorArray const &anchors,
                             Integrator                &&integrator,
                             Remaining&&...            remaining)
    {
        bool more = integrator (positions, anchors);
        more |= PerformIntegration (positions,
                                    anchors,
                                    std::forward <Remaining> (remaining)...);
        return more;
    }

    template <typename... Args>
    bool Integrate (wobbly::MeshArray         &positions,
                    wobbly::AnchorArray const &anchors,
                    unsigned int              steps,
                    Args&&                    ...integrators)
    {
        bool more = false;

        /* Prevent underflow */
        if (!steps)
            return more;

        /* Force is actually going to be something that changes over time
         * depending on how far about the objects are away from each other.
         *
         * Unfortunately that's not a simple thing to model. It requires us to
         * provide an integration of the force function which is in turn
         * dependent on the position of the objects which are in turn dependent
         * on the force applied. That requires implicit integration, which is
         * not dynamic.
         *
         * Its far easier to simply just approximate this by sampling the
         * integration function. The problem that you end up facing then is that
         * for large enough stepsizes (where stepsize > 2 * friction/ k) then
         * you end up with model instability.
         *
         * Having one step every frame is a good approximation although that
         * might need to change in the future */
        do
        {
            more |= PerformIntegration (positions,
                                        anchors,
                                        std::forward <Args> (integrators)...);
        } while (--steps);

        return more;
    }

    template <typename AnchorPoint>
    wobbly::Point TopLeftPositionInSettledMesh (AnchorPoint    const &anchor,
                                                size_t         const index,
                                                wobbly::Vector const &tileSize)
    {
        wobbly::Point start;
        wgd::assign (start, anchor);

        size_t const row = index % wobbly::config::Width;
        size_t const column = index / wobbly::config::Width;

        wobbly::Point deltaToTopLeft (wgd::get <0> (tileSize) * row,
                                      wgd::get <1> (tileSize) * column);
        wgd::pointwise_subtract (start, deltaToTopLeft);

        return start;
    }
}

template <typename... Args>
wobbly::Point
wobbly::Model::Private::TargetPositionByFullIntegration (Args&& ...additionalSteps) const
{
    wobbly::Vector const tileSize (TileSize ());

    auto points (mPositions.PointArray ());
    auto &anchors (mAnchors);

    /* Make our own copies of the integrators and run the integration on them */
    EulerIntegration              integrator (mVelocityIntegrator);
    SpringStep <EulerIntegration> spring (integrator,
                                          points,
                                          mSettings.springConstant,
                                          mSettings.friction,
                                          tileSize);

    /* Keep on integrating this copy until we know the final position */
    while (Integrate (points,
                      anchors,
                      1,
                      spring,
                      std::forward <Args> (additionalSteps)...));

    /* Model will be settled, return the top left point */
    wobbly::Point result;
    wgd::assign (result, wobbly::PointView <double const> (points, 0));

    return result;
}

wobbly::Point
wobbly::Model::Private::TargetPosition () const
{
    /* If we have at least one anchor, we can take a short-cut and determine
     * the target position by reference to it */
    auto early = mTargets.PerformIfActive ([](MeshArray const &targets) {
        wobbly::Point constructed;
        wgd::assign (constructed, wobbly::PointView <double const> (targets, 0));
        return std::make_tuple (true, constructed);
    });

    if (std::get <0> (early))
        return std::get <1> (early);


    ConstrainmentStep constrainment (mConstrainment);
    return TargetPositionByFullIntegration (constrainment);
}

wobbly::Vector
wobbly::Model::Private::TileSize () const
{
    return wobbly::Vector (mWidth / (config::Width - 1),
                           mHeight / (config::Height - 1));
}

namespace
{
    class InsertedSprings
    {
        public:

            typedef wobbly::SpringMesh::AnchorDataVector ADV;

            typedef wobbly::TemporaryOwner <wobbly::Spring> Stolen;
            typedef wobbly::TemporaryOwner <wobbly::Spring::ID> Temporary;
            typedef wobbly::TemporaryOwner <ADV::ID> Anchor;

            InsertedSprings (Stolen                     &&stolen,
                             Temporary                  &&first,
                             Temporary                  &&second,
                             std::unique_ptr <double[]> &&data,
                             Anchor                     &&anchor) :
                stolen (std::move (stolen)),
                first (std::move (first)),
                second (std::move (second)),
                data (std::move (data)),
                anchor (std::move (anchor))
            {
            }

            InsertedSprings (InsertedSprings &&package) noexcept (true):
                stolen (std::move (package.stolen)),
                first (std::move (package.first)),
                second (std::move (package.second)),
                data (std::move (package.data)),
                anchor (std::move (package.anchor))
            {
            }

            InsertedSprings &
            operator= (InsertedSprings &&other) noexcept (true)
            {
                if (this == &other)
                    return *this;

                stolen = std::move (other.stolen);
                first = std::move (other.first);
                second = std::move (other.second);
                data = std::move (other.data);
                anchor = std::move (other.anchor);

                return *this;
            }

            void MoveBy (wobbly::Point const &delta) noexcept
            {
                wobbly::PointView <double> pv (data.get (), 0);
                wgd::pointwise_add (pv, delta);
            }

        private:

            InsertedSprings (InsertedSprings const &) = delete;
            InsertedSprings & operator= (InsertedSprings const &) = delete;

            Stolen stolen;
            Temporary first;
            Temporary second;
            std::unique_ptr <double[]> data;
            Anchor anchor;
    };

    wobbly::Anchor
    InsertPointStrategy (wobbly::TargetMesh::Hnd                       &&handle,
                         wobbly::Point                           const &install,
                         wobbly::MeshArray                       const &points,
                         wobbly::TargetMesh                      const &targets,
                         wobbly::SpringStep <wobbly::EulerIntegration> &spring)
    {
        /* For the first activation, we prefer to use the target positions so
         * that the mesh can eventually settle even while grabbed. For
         * subsequent activations, it doesn't make any sense to have a settling
         * mesh while those grabs are active, since it could be impossible. In
         * such a case, just grab on the real positions */
        using namespace wobbly;

        typedef PointView <double const> const & (Spring::*PosFetch) () const;

        auto const getTarget = [&points, &targets](Spring const &spring,
                                                   PosFetch fetch) {
            return targets.PerformIfActive ([](MeshArray const &targets,
                                               MeshArray const &points,
                                               Spring    const &spring,
                                               PosFetch        fetch) {
                /* Lookup the index of each of the positions referenced in the
                 * spring and then fetch from the target array */
                // cppcheck-suppress unreachableCode
                for (size_t i = 0; i < config::TotalIndices; ++i)
                {
                    PointView <double const> position (points, i);

                    /* We can't return a PointView direclty since it isn't
                     * default-constructible, but we can return a tuple
                     * with its arguments */
                    if (wgd::equals ((spring.*fetch) (), position))
                        return std::make_tuple (targets.data (), i);
                }

                /* This is a bug */
                throw std::runtime_error ("Couldn't find position in mesh");
            }, points, spring, fetch);
        };

        using namespace std::placeholders;
        auto const wrap =
            [&targets, &getTarget](PosFetch fetch) {
                bool active = targets.PerformIfActive ([](MeshArray const &) {
                    return true;
                });

                typedef SpringMesh::PosPreference PP;

                /* If the target mesh is "active" (eg, there is one and only
                 * one grab on it, then we insert the anchor as having a
                 * desired distance between the two target mesh positions
                 * that it will eventually rest between. This ensures that
                 * if we ungrabbed the mesh completely and then re-grabbed it,
                 * the mesh will still eventually settle.
                 *
                 * If the mesh is not active, then the desired distance is the
                 * current distance between the inserted point and the actual
                 * points on the mesh. The mesh will never settle while
                 * the grab is held, but that's fine because it wasn't going
                 * to settle anyways */
                return active ? PP ([fetch, &getTarget](Spring const &spring) {
                                        // cppcheck-suppress unreachableCode
                                        auto args = getTarget (spring, fetch);
                                        typedef wobbly::PointView <double const>
                                                CDPV;
                                        return CDPV (std::get <0> (args),
                                                     std::get <1> (args));
                                    }) :
                                PP ([fetch](Spring const &spring) {
                                        return (spring.*fetch) ();
                                    });
            };

        SpringMesh::PosPreference firstPref (wrap (&Spring::FirstPosition));
        SpringMesh::PosPreference secondPref (wrap (&Spring::SecondPosition));

        auto result (spring.InstallAnchorSprings (install,
                                                  firstPref,
                                                  secondPref));

        typedef InsertedSprings IS;

        /* XXX: There does not appear to be any freely-available
         * header-only libraries which permit functional
         * type apply () of the arguments of an std::tuple to
         * a function or constructor */
        using Impl = wobbly::Anchor::Impl;
        Impl impl (new ConstrainingAnchor <IS> (std::move (handle),
                                                std::move (result.stolen),
                                                std::move (result.first),
                                                std::move (result.second),
                                                std::move (result.data),
                                                std::move (result.anchor)));
        return wobbly::Anchor::Create (std::move (impl));
    }

    class GrabAnchor
    {
        public:

            GrabAnchor (wobbly::PointView <double> &&position,
                        wobbly::AnchorArray        &array,
                        size_t                     index) :
                position (std::move (position)),
                array (array),
                index (index)
            {
                array.Lock (index);
            }

            ~GrabAnchor ()
            {
                array.Unlock (index);
            }

            void MoveBy (wobbly::Point const &delta) noexcept
            {
                wgd::pointwise_add (position, delta);
            }

        private:

            GrabAnchor (GrabAnchor const &) = delete;
            GrabAnchor & operator= (GrabAnchor const &) = delete;

            wobbly::PointView <double> position;
            wobbly::AnchorArray        &array;
            size_t                     index;
    };

    wobbly::Anchor
    GrabAnchorStrategy (wobbly::TargetMesh::Hnd    &&handle,
                        wobbly::PointView <double> &&point,
                        wobbly::AnchorArray        &anchors,
                        size_t                     index)
    {
        typedef GrabAnchor GA;

        using Impl = wobbly::Anchor::Impl;
        Impl impl (new wobbly::ConstrainingAnchor <GA> (std::move (handle),
                                                        std::move (point),
                                                        anchors,
                                                        index));

        return wobbly::Anchor::Create (std::move (impl));
    }
}

wobbly::Anchor
wobbly::Model::GrabAnchor (Point const &position) noexcept (false)
{
    auto &points = priv->mPositions.PointArray ();
    size_t index = mesh::ClosestIndexToPosition (points, position);

    /* Bets are off once we've grabbed an anchor, the model is now unequal */
    priv->mCurrentlyUnequal = true;

    /* Activate the TargetMesh member for this anchor */
    auto activation (priv->mTargets.Activate ());

    return Anchor (GrabAnchorStrategy (std::move (activation),
                                       wobbly::PointView <double> (points,
                                                                   index),
                                       priv->mAnchors,
                                       index));
}

wobbly::Anchor
wobbly::Model::InsertAnchor (Point const &position) noexcept (false)
{
    auto &points = priv->mPositions.PointArray ();

    /* Bets are off once we've inserted an anchor, the model is now unequal */
    priv->mCurrentlyUnequal = true;

    /* Activate the TargetMesh member for this anchor */
    auto activation (priv->mTargets.Activate ());

    return Anchor (InsertPointStrategy (std::move (activation),
                                        position,
                                        points,
                                        priv->mTargets,
                                        priv->mSpring));
}

void
wobbly::Model::MoveModelBy (Point const &delta)
{
    auto &points (priv->mPositions.PointArray ());
    auto &estimated (priv->mTargets.PointArray ());

    for (size_t i = 0; i < config::TotalIndices; ++i)
    {
        PointView <double> pointView (points, i);
        PointView <double> targetView (estimated, i);
        wgd::pointwise_add (pointView, delta);
        wgd::pointwise_add (targetView, delta);
    }

    /* Also move any inserted springs */
    priv->mSpring.MoveInsertedAnchorsBy (delta);
}

void
wobbly::Model::MoveModelTo (Point const &point)
{
    /* We need to calculate the target position for the
     * top left corner. If we do that, then moving the model
     * relative to that will ensure that it settles in the
     * place that we expect. */

    auto const &target (priv->TargetPosition ());

    Vector delta (point);
    wgd::pointwise_subtract (delta, target);

    MoveModelBy (delta);
}

void
wobbly::Model::ResizeModel (double width, double height)
{
    /* First, zero or negative widths are invalid */
    assert (width > 0.0f);
    assert (height > 0.0f);

    /* Second, work out the scale factors */
    wobbly::Vector const scaleFactor (width / priv->mWidth,
                                      height / priv->mHeight);

    if (wgd::equals (scaleFactor, wobbly::Vector (1.0, 1.0)))
        return;

    /* Then on each point, implement a transformation
     * for non-anchors that scales the distance between
     * points in model space */
    auto &points (priv->mPositions.PointArray ());
    auto &targets (priv->mTargets.PointArray ());

    wobbly::Point const positionsOrigin (priv->TargetPosition ());
    wobbly::Point const targetsOrigin = [&targets]() {
        wobbly::Point target;
        wgd::assign (target, wobbly::PointView <double const> (targets, 0));
        return target;
    } ();

    auto const rescale =
        [&scaleFactor](Point const &origin, PointView <double> &&p) {
            wgd::pointwise_subtract (p, origin);
            wgd::pointwise_scale (p, scaleFactor);
            wgd::pointwise_add (p, origin);
        };

    /* Rescale all points and targets */
    for (size_t i = 0; i < config::TotalIndices; ++i)
    {
        rescale (positionsOrigin, PointView <double> (points, i));
        rescale (targetsOrigin, PointView <double> (targets, i));
    }

    /* On each spring, apply the scale factor */
    priv->mSpring.Scale (positionsOrigin, scaleFactor);

    /* Apply width and height changes */
    priv->mWidth = width;
    priv->mHeight = height;
}

wobbly::ConstrainmentStep::ConstrainmentStep (double     const &threshold,
                                              TargetMesh const &targets) :
    threshold (threshold),
    targets (targets)
{
}

bool
wobbly::ConstrainmentStep::operator () (MeshArray         &points,
                                        AnchorArray const &anchors)
{
    /* If an anchor is grabbed, then the model will be considered constrained.
     * The first anchor taking priority - we work out the allowable range for
     * each spring and then apply correction as appropriate before even
     * starting to integrate the model
     */
    return targets.PerformIfActive ([this, &points](MeshArray const &targets) {
        // cppcheck-suppress unreachableCode
        bool ret = false;

        for (size_t i = 0; i < config::TotalIndices; ++i)
        {
            wobbly::PointView <double const> target (targets, i);
            /* In each position in the main position array we'll work out the
             * pythagorean delta between the ideal positon and current one.
             * If it is outside the maximum range, then we'll shrink the delta
             * and reapply it */
            double const maximumRange = threshold;

            wobbly::PointView <double> point (points, i);
            double range = wgd::distance (target, point);

            if (range < maximumRange)
                continue;

            ret |= true;

            auto sin = (wgd::get <1> (target) - wgd::get <1> (point)) / range;
            auto cos = (wgd::get <0> (target) - wgd::get <0> (point)) / range;

            /* Now we want to vectorize range and
             * find our new x and y offsets */
            double const newRange = std::min (maximumRange, range);
            wobbly::Point newDelta (newRange * cos,
                                    newRange * sin);

            /* Offset from the "target" position */
            wgd::assign (point, target);
            wgd::pointwise_subtract (point, newDelta);
        }

        return ret;
    });
}

wobbly::EulerIntegration::EulerIntegration ()
{
    velocities.fill (0.0);
}

bool
wobbly::Model::Step (unsigned int time)
{
    bool moreStepsRequired = priv->mCurrentlyUnequal;

    double const FPStepResolution = 16.0f;
    unsigned int steps =
        static_cast <unsigned int> (std::ceil (time / FPStepResolution));

    /* We might not need more steps - set to false initially and then
     * integrate the model to see if we do */
    if (time)
        moreStepsRequired = false;

    moreStepsRequired |= Integrate (priv->mPositions.PointArray (),
                                    priv->mAnchors,
                                    steps,
                                    priv->mConstrainment,
                                    priv->mSpring);

    priv->mCurrentlyUnequal = moreStepsRequired;

    /* If we've settled and have grabbed anchors, snap to the mesh resting
     * point where the cursor is, this ensures exact positioning */
    if (!priv->mCurrentlyUnequal)
    {
        auto       &positions (priv->mPositions.PointArray ());

        priv->mTargets.PerformIfActive ([&positions](MeshArray const &targets) {
            std::copy (targets.begin (), targets.end (), positions.begin ());
        });
    }

    return priv->mCurrentlyUnequal;
}

wobbly::Point
wobbly::Model::DeformTexcoords (Point const &normalized) const
{
    return priv->mPositions.DeformUnitCoordsToMeshSpace (normalized);
}

std::array <wobbly::Point, 4> const
wobbly::Model::Extremes () const
{
    return priv->mPositions.Extremes ();
}

wobbly::TargetMesh::TargetMesh (OriginRecalcStrategy const &origin) :
    activationCount (0),
    origin (origin)
{
    mPoints.fill (0);
}

wobbly::TargetMesh::Hnd
wobbly::TargetMesh::Activate () noexcept (true)
{
    /* Recompute where all the targets would be if we have
     * a single anchor. */
    if (++activationCount == 1)
        origin (mPoints);

    Move moveBy = [this](Vector const &delta) {
        if (Active ())
        {
            for (size_t i = 0; i < config::TotalIndices; ++i)
            {
                PointView <double> pv (mPoints, i);
                wgd::pointwise_add (pv, delta);
            }
        }
    };

    return Hnd (MakeMoveOnly (std::move (moveBy)),
                [this](MoveOnly <Move> &&handle) {
                    /* Anchor count dropped to 1, but the relevant
                     * anchor could have changed. Recompute where
                     * all the targets are. */
                    if (--activationCount == 1)
                        origin (mPoints);
                });
}

wobbly::BezierMesh::BezierMesh ()
{
    mPoints.fill (0.0);
}

wobbly::BezierMesh::~BezierMesh ()
{
}

namespace
{
    template <typename Point>
    void PointRound (Point &point)
    {
        wgd::for_each_coordinate (point, [](auto const &c) {
            return std::round (c);
        });
    }

    template <typename Numeric>
    struct NumericTraits
    {
        typedef Numeric (*Chooser) (Numeric, Numeric);
    };

    template <typename Numeric>
    void SetToExtreme (wobbly::Point                             &p,
                       Numeric                                   x,
                       typename NumericTraits <Numeric>::Chooser xFinder,
                       Numeric                                   y,
                       typename NumericTraits <Numeric>::Chooser yFinder)
    {
        wgd::set <0> (p, xFinder (wgd::get <0> (p), x));
        wgd::set <1> (p, yFinder (wgd::get <1> (p), y));

        /* Round to next integer */
        PointRound (p);
    }

    unsigned int CoordIndex (size_t x,
                             size_t y,
                             unsigned int width)
    {
        return y * width + x;
    }
}

std::array <wobbly::Point, 4> const
wobbly::BezierMesh::Extremes () const
{
    double const maximum = std::numeric_limits <double>::max ();
    double const minimum = std::numeric_limits <double>::lowest ();

    std::array <wobbly::Point, 4> extremes =
    {
        {
            wobbly::Point (maximum, maximum),
            wobbly::Point (minimum, maximum),
            wobbly::Point (maximum, minimum),
            wobbly::Point (minimum, minimum)
        }
    };

    wobbly::Point &topLeft (extremes[0]);
    wobbly::Point &topRight (extremes[1]);
    wobbly::Point &bottomLeft (extremes[2]);
    wobbly::Point &bottomRight (extremes[3]);

    auto min = [](double lhs, double rhs) -> double {
        double result =  std::min (lhs, rhs);
        return result;
    };

    auto max = [](double lhs, double rhs) -> double {
        double result = std::max (lhs, rhs);
        return result;
    };

    for (size_t i = 0; i < config::Width * config::Height * 2; i += 2)
    {
        double const x = mPoints[i];
        double const y = mPoints[i + 1];

        SetToExtreme (topLeft, x, min, y, min);
        SetToExtreme (topRight, x, max, y, min);
        SetToExtreme (bottomLeft, x, min, y, max);
        SetToExtreme (bottomRight, x, max, y, max);
    }

    return extremes;
}

wobbly::PointView <double>
wobbly::BezierMesh::PointForIndex (size_t x, size_t y)
{
    return wobbly::PointView <double> (mPoints,
                                       CoordIndex (x, y,
                                                   config::Width));
}
