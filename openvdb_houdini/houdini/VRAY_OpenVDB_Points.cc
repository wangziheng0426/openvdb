///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2012-2016 DreamWorks Animation LLC
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DreamWorks Animation nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////
//
/// @file VRAY_OpenVDB_Points.cc
///
/// @authors Dan Bailey, Richard Kwok
///
/// @brief The Delayed Load Mantra Procedural for OpenVDB Points.


#include <UT/UT_DSOVersion.h>
#include <GU/GU_Detail.h>
#include <OP/OP_OperatorTable.h>
#include <UT/UT_BoundingBox.h>
#include <VRAY/VRAY_Procedural.h>

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>

#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointGroup.h>

#include "PointUtils.h"

using namespace openvdb;
using namespace openvdb::points;

namespace hvdb = openvdb_houdini;

// mantra renders points with a world-space radius of 0.05 by default
static const float DEFAULT_PSCALE = 0.05f;

class VRAY_OpenVDB_Points : public VRAY_Procedural {
public:
    VRAY_OpenVDB_Points();
    virtual ~VRAY_OpenVDB_Points() = default;

    virtual const char  *className() const override;

    virtual int      initialize(const UT_BoundingBox *) override;
    virtual void     getBoundingBox(UT_BoundingBox &box) override;
    virtual void     render() override;

private:
    UT_BoundingBox                              mBox;
    UT_String                                   mFilename;
    std::vector<Name>                           mIncludeGroups;
    std::vector<Name>                           mExcludeGroups;
    UT_String                                   mAttrStr;
    std::vector<PointDataGrid::Ptr>             mGridPtrs;
    float                                       mPreBlur;
    float                                       mPostBlur;
    bool                                        mSpeedToColor;
    float                                       mMaxSpeed;
    UT_Ramp                                     mFunctionRamp;

}; // class VRAY_OpenVDB_Points

////////////////////////////////////////

template <typename PointDataTreeT>
struct GenerateBBoxOp {

    using PointDataLeaf = typename PointDataTreeT::LeafNodeType;
    using LeafRangeT    = typename tree::LeafManager<const PointDataTreeT>::LeafRange;

    GenerateBBoxOp( const math::Transform& transform,
                    const std::vector<Name>& includeGroups,
                    const std::vector<Name>& excludeGroups)
        : mTransform(transform)
        , mIncludeGroups(includeGroups)
        , mExcludeGroups(excludeGroups) { }

    GenerateBBoxOp(const GenerateBBoxOp& parent, tbb::split)
        : mTransform(parent.mTransform)
        , mBbox(parent.mBbox)
        , mIncludeGroups(parent.mIncludeGroups)
        , mExcludeGroups(parent.mExcludeGroups) { }

    void operator()(const LeafRangeT& range) {

        for (auto leafIter = range.begin(); leafIter; ++leafIter) {

            const AttributeSet::Descriptor& descriptor = leafIter->attributeSet().descriptor();

            size_t pscaleIndex = descriptor.find("pscale");
            if (pscaleIndex != AttributeSet::INVALID_POS) {

                std::string pscaleType = descriptor.type(pscaleIndex).first;

                if (pscaleType == typeNameAsString<float>())        expandBBox<float>(*leafIter, pscaleIndex);
                else if (pscaleType == typeNameAsString<half>())    expandBBox<half>(*leafIter, pscaleIndex);
                else {
                    throw TypeError("Unsupported pscale type - " + pscaleType);
                }
            }
        }
    }

    void join(GenerateBBoxOp& rhs) {
        mBbox.expand(rhs.mBbox);
    }

    template <typename PscaleType>
    void expandBBox(const PointDataLeaf& leaf, size_t pscaleIndex) {

        auto positionHandle = points::AttributeHandle<Vec3f>::create(leaf.constAttributeArray("P"));

        // expandBBox will not pick up a pscale handle unless the attribute type matches the template type

        typename AttributeHandle<PscaleType>::Ptr pscaleHandle;
        if (pscaleIndex != AttributeSet::INVALID_POS) {
            if (leaf.attributeSet().descriptor().type(pscaleIndex).first == typeNameAsString<PscaleType>()) {
                pscaleHandle = AttributeHandle<PscaleType>::create(leaf.constAttributeArray(pscaleIndex));
            }
        }

        // uniform value is in world space
        bool pscaleIsUniform = true;
        PscaleType uniformPscale(DEFAULT_PSCALE);

        if (pscaleHandle) {
            pscaleIsUniform = pscaleHandle->isUniform();
            uniformPscale = pscaleHandle->get(0);
        }

        // combine the bounds of every point on this leaf into an index-space bbox

        if (!mIncludeGroups.empty() || !mExcludeGroups.empty()) {

            points::MultiGroupFilter filter(mIncludeGroups, mExcludeGroups);
            auto iter = leaf.beginIndexOn(filter);

            for (; iter; ++iter) {

                double pscale = double(pscaleIsUniform ? uniformPscale : pscaleHandle->get(*iter));

                // pscale needs to be transformed to index space
                Vec3d radius = mTransform.worldToIndex(Vec3d(pscale));
                Vec3d position = iter.getCoord().asVec3d() + positionHandle->get(*iter);

                mBbox.expand(position - radius);
                mBbox.expand(position + radius);
            }
        }
        else {

            auto iter = leaf.beginIndexOn();

            for (; iter; ++iter) {

                double pscale = double(pscaleIsUniform ? uniformPscale : pscaleHandle->get(*iter));

                // pscale needs to be transformed to index space
                Vec3d radius = mTransform.worldToIndex(Vec3d(pscale));
                Vec3d position = iter.getCoord().asVec3d() + positionHandle->get(*iter);

                mBbox.expand(position - radius);
                mBbox.expand(position + radius);

            }
        }
    }

    /////////////

    const math::Transform&      mTransform;
    BBoxd                       mBbox;
    const std::vector<Name>&    mIncludeGroups;
    const std::vector<Name>&    mExcludeGroups;

}; // GenerateBBoxOp

//////////////////////////////////////


template <typename PointDataTreeT, typename ColorVec3T, typename VelocityVec3T>
struct PopulateColorFromVelocityOp {

    using LeafNode          = typename PointDataTreeT::LeafNodeType;
    using IndexOnIter       = typename LeafNode::IndexOnIter;
    using LeafManagerT      = typename tree::LeafManager<PointDataTreeT>;
    using LeafRangeT        = typename LeafManagerT::LeafRange;
    using MultiGroupFilter  = points::MultiGroupFilter;

    PopulateColorFromVelocityOp(    const size_t colorIndex,
                                    const size_t velocityIndex,
                                    const UT_Ramp& ramp,
                                    const float maxSpeed,
                                    const std::vector<Name>& includeGroups,
                                    const std::vector<Name>& excludeGroups,
                                    const bool collapseVelocityAfter)
        : mColorIndex(colorIndex)
        , mVelocityIndex(velocityIndex)
        , mRamp(ramp)
        , mMaxSpeed(maxSpeed)
        , mIncludeGroups(includeGroups)
        , mExcludeGroups(excludeGroups)
        , mCollapseVelocityAfter(collapseVelocityAfter) { }

    ColorVec3T getColorFromRamp(const VelocityVec3T& velocity) const{

        float proportionalSpeed = (mMaxSpeed == 0.0f ? 0.0f : velocity.length()/mMaxSpeed);

        if (proportionalSpeed > 1.0f)   proportionalSpeed = 1.0f;
        if (proportionalSpeed < 0.0f)   proportionalSpeed = 0.0f;

        float rampVal[4];
        mRamp.rampLookup(proportionalSpeed, rampVal);
        return ColorVec3T(rampVal[0], rampVal[1], rampVal[2]);
    }

    void operator()(LeafRangeT& range) const{

        for (auto leafIter = range.begin(); leafIter; ++leafIter) {

            auto& leaf = *leafIter;

            auto colorHandle = points::AttributeWriteHandle<ColorVec3T>::create(leaf.attributeArray(mColorIndex));

            auto velocityHandle = points::AttributeWriteHandle<VelocityVec3T>::create(leaf.attributeArray(mVelocityIndex));

            const bool uniform = velocityHandle->isUniform();
            const ColorVec3T uniformColor = getColorFromRamp(velocityHandle->get(0));

            if (!mIncludeGroups.empty() || !mExcludeGroups.empty()) {

                MultiGroupFilter filter(mIncludeGroups, mExcludeGroups);
                auto iter = leaf.beginIndexOn(filter);

                for (; iter; ++iter) {

                    ColorVec3T color = uniform ? uniformColor : getColorFromRamp(velocityHandle->get(*iter));
                    colorHandle->set(*iter, color);
                }
            }
            else {

                auto iter = leaf.beginIndexOn();

                for (; iter; ++iter) {

                    ColorVec3T color = uniform ? uniformColor : getColorFromRamp(velocityHandle->get(*iter));
                    colorHandle->set(*iter, color);
                }
            }

            if (mCollapseVelocityAfter)     velocityHandle->collapse(VelocityVec3T(0));
        }
    }

    //////////////////////////////////////////////

    const size_t                            mColorIndex;
    const size_t                            mVelocityIndex;
    const UT_Ramp&                          mRamp;
    const float                             mMaxSpeed;
    const std::vector<Name>&                mIncludeGroups;
    const std::vector<Name>&                mExcludeGroups;
    const bool                              mCollapseVelocityAfter;
};

////////////////////////////////////////////


namespace {

template <typename PointDataGridT>
inline BBoxd
getBoundingBox( const std::vector<typename PointDataGridT::Ptr>& gridPtrs,
                const std::vector<Name>& includeGroups,
                const std::vector<Name>& excludeGroups)
{
    using PointDataTree     = typename PointDataGridT::TreeType;
    using PointDataGridPtr  = typename PointDataGridT::Ptr;

    BBoxd worldBounds;

    for (const auto grid : gridPtrs) {

        tree::LeafManager<const PointDataTree> leafManager(grid->tree());

        // size and combine the boxes for each leaf in the tree via a reduction
        GenerateBBoxOp<PointDataTree> generateBbox(grid->transform(), includeGroups, excludeGroups);
        tbb::parallel_reduce(leafManager.leafRange(), generateBbox);

        if (generateBbox.mBbox.empty())     continue;

        // all the bounds must be unioned in world space
        BBoxd gridBounds = grid->transform().indexToWorld(generateBbox.mBbox);
        worldBounds.expand(gridBounds);
    }

    return worldBounds;
}

} // namespace

static VRAY_ProceduralArg   theArgs[] = {
    VRAY_ProceduralArg("file", "string", ""),
    VRAY_ProceduralArg("groupmask", "string", ""),
    VRAY_ProceduralArg("attrmask", "string", ""),
    VRAY_ProceduralArg("speedtocolor", "int", "0"),
    VRAY_ProceduralArg("maxspeed", "real", "1.0"),
    VRAY_ProceduralArg("ramp", "string", ""),
    VRAY_ProceduralArg()
};

VRAY_Procedural *
allocProcedural(const char *)
{
    return new VRAY_OpenVDB_Points();
}

const VRAY_ProceduralArg *
getProceduralArgs(const char *)
{
    return theArgs;
}

VRAY_OpenVDB_Points::VRAY_OpenVDB_Points()
{
    openvdb::initialize();
}

const char *
VRAY_OpenVDB_Points::className() const
{
    return "VRAY_OpenVDB_Points";
}

int
VRAY_OpenVDB_Points::initialize(const UT_BoundingBox *)
{

    import("file", mFilename);
    import("attrmask", mAttrStr);

    float fps;
    import("global:fps", &fps, 1);

    float shutter[2];
    import("camera:shutter", shutter, 2);

    int velocityBlur;
    import("object:velocityblur", &velocityBlur, 1);

    mPreBlur = velocityBlur ? -shutter[0]/fps : 0;
    mPostBlur = velocityBlur ? shutter[1]/fps : 0;

    int speedToColorInt = 0;
    import("speedtocolor", &speedToColorInt, 1);
    mSpeedToColor = bool(speedToColorInt);

    // if speed-to-color is enabled we need to build a ramp object
    if (mSpeedToColor) {

        import("maxspeed", &mMaxSpeed, 1);

        UT_String rampStr;
        import("ramp", rampStr);

        std::stringstream rampStream(rampStr.toStdString());
        std::istream_iterator<float> begin(rampStream);
        std::istream_iterator<float> end;
        std::vector<float> rampVals(begin, end);

        for (size_t n = 4, N = rampVals.size(); n < N; n += 5) {
            mFunctionRamp.addNode(rampVals[n-4], UT_FRGBA(rampVals[n-3], rampVals[n-2], rampVals[n-1], 1.0f),
                static_cast<UT_SPLINE_BASIS>(rampVals[n]));
        }
    }

    // save the grids so that we only read the file once
    try
    {
        io::File file(mFilename.toStdString());
        file.open();

        for (auto iter = file.beginName(), endIter = file.endName(); iter != endIter; ++iter) {

            GridBase::Ptr baseGrid = file.readGridMetadata(*iter);
            if (baseGrid->isType<points::PointDataGrid>()) {
                auto grid = StaticPtrCast<points::PointDataGrid>(file.readGrid(*iter));
                assert(grid);
                mGridPtrs.push_back(grid);
            }
        }

        file.close();
    }
    catch (const IoError& e)
    {
        OPENVDB_LOG_ERROR(e.what() << " (" << mFilename << ")");
        return 0;
    }

    // extract which groups to include and exclude
    UT_String groupStr;
    import("groupmask", groupStr);
    AttributeSet::Descriptor::parseNames(mIncludeGroups, mExcludeGroups, groupStr.toStdString());

    // get openvdb bounds and convert to houdini bounds
    BBoxd vdbBox = ::getBoundingBox<PointDataGrid>(mGridPtrs, mIncludeGroups, mExcludeGroups);
    mBox.setBounds(vdbBox.min().x(), vdbBox.min().y(), vdbBox.min().z()
                  ,vdbBox.max().x(), vdbBox.max().y(), vdbBox.max().z());

    return 1;
}

void
VRAY_OpenVDB_Points::getBoundingBox(UT_BoundingBox &box)
{
    box = mBox;
}

void
VRAY_OpenVDB_Points::render()
{
    using PointDataTree     = points::PointDataGrid::TreeType;
    using PointDataGridPtr  = points::PointDataGrid::Ptr;
    using AttributeSet      = points::AttributeSet;
    using Descriptor        = AttributeSet::Descriptor;

    /// Allocate geometry and extract the GU_Detail
    VRAY_ProceduralGeo  geo = createGeometry();

    GU_Detail* gdp = geo.get();

    // extract which attributes to include and exclude
    std::vector<Name> includeAttributes;
    std::vector<Name> excludeAttributes;
    AttributeSet::Descriptor::parseNames(includeAttributes, excludeAttributes, mAttrStr.toStdString());

    // if nothing was included or excluded: "all attributes" is implied with an empty vector
    // if nothing was included but something was explicitly excluded: add all attributes but then remove the excluded
    // if something was included: add only explicitly included attributes and then removed any excluded

    if (includeAttributes.empty() && !excludeAttributes.empty()) {

        // add all attributes
        for (const auto grid : mGridPtrs) {

            auto leafIter = grid->tree().cbeginLeaf();
            if (!leafIter) continue;

            const AttributeSet& attributeSet = leafIter->attributeSet();
            const Descriptor& descriptor = attributeSet.descriptor();
            const Descriptor::NameToPosMap& nameToPosMap = descriptor.map();

            for (const auto& namePos : nameToPosMap) {

                includeAttributes.push_back(namePos.first);
            }
        }
    }

    // sort, and then remove any duplicates
    std::sort(includeAttributes.begin(), includeAttributes.end());
    std::sort(excludeAttributes.begin(), excludeAttributes.end());
    includeAttributes.erase(std::unique(includeAttributes.begin(), includeAttributes.end()), includeAttributes.end());
    excludeAttributes.erase(std::unique(excludeAttributes.begin(), excludeAttributes.end()), excludeAttributes.end());

    // make a vector (validAttributes) of all elements that are in includeAttributes but are NOT in excludeAttributes
    std::vector<Name> validAttributes(includeAttributes.size());
    auto pastEndIter = std::set_difference(includeAttributes.begin(), includeAttributes.end(),
        excludeAttributes.begin(), excludeAttributes.end(), validAttributes.begin());
    validAttributes.resize(pastEndIter - validAttributes.begin());

    // if any of the grids are going to add a pscale, set the default here
    if (std::binary_search(validAttributes.begin(), validAttributes.end(), "pscale")) {
        gdp->addTuple(GA_STORE_REAL32, GA_ATTRIB_POINT, "pscale", 1, GA_Defaults(DEFAULT_PSCALE));
    }

    // map speed to color if requested
    if (mSpeedToColor) {
        for (const auto grid : mGridPtrs) {

            PointDataTree& tree = grid->tree();

            auto leafIter = tree.beginLeaf();
            if (!leafIter) continue;

            size_t velocityIndex = leafIter->attributeSet().find("v");
            if (velocityIndex != AttributeSet::INVALID_POS) {

                const std::string velocityType = leafIter->attributeSet().descriptor().type(velocityIndex).first;

                // keep existing Cd attribute only if it is a supported type (float or half)
                size_t colorIndex = leafIter->attributeSet().find("Cd");
                std::string colorType = "";
                if (colorIndex != AttributeSet::INVALID_POS) {
                    colorType = leafIter->attributeSet().descriptor().type(colorIndex).first;
                    if (colorType != typeNameAsString<Vec3f>() && colorType != typeNameAsString<Vec3H>()) {
                        dropAttribute(tree, "Cd");
                        colorIndex = AttributeSet::INVALID_POS;
                    }
                }

                // create new Cd attribute of supported type if one did not previously exist
                if (colorIndex == AttributeSet::INVALID_POS) {
                    openvdb::points::appendAttribute<Vec3H>(tree, "Cd");
                    colorIndex = leafIter->attributeSet().find("Cd");
                }

                tree::LeafManager<PointDataTree> leafManager(tree);

                bool collapseVelocityAfter =
                    !validAttributes.empty() && !std::binary_search(validAttributes.begin(), validAttributes.end(), "v");

                if (colorType == typeNameAsString<Vec3f>() && velocityType == typeNameAsString<Vec3f>()) {
                    PopulateColorFromVelocityOp<PointDataTree, Vec3f, Vec3f> populateColor(colorIndex, velocityIndex,
                        mFunctionRamp, mMaxSpeed, mIncludeGroups, mExcludeGroups, collapseVelocityAfter);
                    tbb::parallel_for(leafManager.leafRange(), populateColor);
                }
                else if (colorType == typeNameAsString<Vec3f>() && velocityType == typeNameAsString<Vec3H>()) {
                    PopulateColorFromVelocityOp<PointDataTree, Vec3f, Vec3H> populateColor(colorIndex, velocityIndex,
                        mFunctionRamp, mMaxSpeed, mIncludeGroups, mExcludeGroups, collapseVelocityAfter);
                    tbb::parallel_for(leafManager.leafRange(), populateColor);
                }
                else if (colorType == typeNameAsString<Vec3H>() && velocityType == typeNameAsString<Vec3f>()) {
                    PopulateColorFromVelocityOp<PointDataTree, Vec3H, Vec3f> populateColor(colorIndex, velocityIndex,
                        mFunctionRamp, mMaxSpeed, mIncludeGroups, mExcludeGroups, collapseVelocityAfter);
                    tbb::parallel_for(leafManager.leafRange(), populateColor);
                }
                else if (colorType == typeNameAsString<Vec3H>() && velocityType == typeNameAsString<Vec3H>()) {
                    PopulateColorFromVelocityOp<PointDataTree, Vec3H, Vec3H> populateColor(colorIndex, velocityIndex,
                        mFunctionRamp, mMaxSpeed, mIncludeGroups, mExcludeGroups, collapseVelocityAfter);
                    tbb::parallel_for(leafManager.leafRange(), populateColor);
                }
            }
        }
    }

    for (const auto grid : mGridPtrs) {

        hvdb::convertPointDataGridToHoudini(*gdp, *grid, validAttributes, mIncludeGroups, mExcludeGroups);
    }

    geo.addVelocityBlur(mPreBlur, mPostBlur);

    // Create a geometry object in mantra
    VRAY_ProceduralChildPtr obj = createChild();
    obj->addGeometry(geo);

    // Override the renderpoints setting to always enable points only rendering
    obj->changeSetting("renderpoints", "true");
}

////////////////////////////////////////

// Copyright (c) 2012-2016 DreamWorks Animation LLC
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )