/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "TileNode"
#include "SurfaceNode"
#include "ProxySurfaceNode"
#include "EngineContext"
#include "Loader"
#include "LoadTileData"
#include "SelectionInfo"
#include "ElevationTextureUtils"
#include "TerrainCuller"

#include <osgEarth/CullingUtils>
#include <osgEarth/ImageUtils>
#include <osgEarth/TraversalData>
#include <osgEarth/Shadowing>
#include <osgEarth/Utils>
#include <osgEarth/TraversalData>

#include <osg/Uniform>
#include <osg/ComputeBoundsVisitor>
#include <osg/ValueObject>

using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;

#define OSGEARTH_TILE_NODE_PROXY_GEOMETRY_DEBUG 0

// Whether to check the child nodes for culling before traversing them.
// This could prevent premature Loader requests, but it increases cull time.
//#define VISIBILITY_PRECHECK

#define LC "[TileNode] "

#define REPORT(name,timer) if(context->progress()) { \
    context->progress()->stats()[name] += OE_GET_TIMER(timer); }

namespace
{
    // Scale and bias matrices, one for each TileKey quadrant.
    const osg::Matrixf scaleBias[4] =
    {
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.0f,0.5f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.5f,0.5f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.0f,0.0f,0,1.0f),
        osg::Matrixf(0.5f,0,0,0, 0,0.5f,0,0, 0,0,1.0f,0, 0.5f,0.0f,0,1.0f)
    };
}

TileNode::TileNode() : 
_dirty        ( false ),
_childrenReady( false ),
_minExpiryTime( 0.0 ),
_minExpiryFrames( 0 ),
_lastTraversalTime(0.0),
_lastTraversalFrame(0.0),
_count(0),
_stitchNormalMap(false)
{
    //nop
}

void
TileNode::create(const TileKey& key, TileNode* parent, EngineContext* context)
{
    if (!context)
        return;

    _context = context;

    _key = key;

    // whether the stitch together normal maps for adjacent tiles.
    _stitchNormalMap = context->_options.normalizeEdges() == true;

    // Encode the tile key in a uniform. Note! The X and Y components are presented
    // modulo 2^16 form so they don't overrun single-precision space.
    unsigned tw, th;
    _key.getProfile()->getNumTiles(_key.getLOD(), tw, th);

    const double m = 65536; //pow(2.0, 16.0);

    double x = (double)_key.getTileX();
    double y = (double)(th - _key.getTileY()-1);

    _tileKeyValue.set(
        (float)fmod(x, m),
        (float)fmod(y, m),
        (float)_key.getLOD(),
        -1.0f);

    // Mask generator creates geometry from masking boundaries when they exist.
    osg::ref_ptr<MaskGenerator> masks = new MaskGenerator(
        key, 
        context->getOptions().tileSize().get(),
        context->getMap());

    MapInfo mapInfo(context->getMap());

    // Get a shared geometry from the pool that corresponds to this tile key:
    osg::ref_ptr<osg::Geometry> geom;
    context->getGeometryPool()->getPooledGeometry(key, mapInfo, geom, masks.get());

    // Create the drawable for the terrain surface:
    TileDrawable* surfaceDrawable = new TileDrawable(
        key, 
        geom.get(),
        context->getOptions().tileSize().get() );

    // Give the tile Drawable access to the render model so it can properly
    // calculate its bounding box and sphere.
    surfaceDrawable->setModifyBBoxCallback(context->getModifyBBoxCallback());

    // Create the node to house the tile drawable:
    _surface = new SurfaceNode(
        key,
        mapInfo,
        context->getRenderBindings(),
        surfaceDrawable );

    // initialize all the per-tile uniforms the shaders will need:
    float start = (float)context->getSelectionInfo().visParameters(_key.getLOD())._fMorphStart;
    float end   = (float)context->getSelectionInfo().visParameters(_key.getLOD())._fMorphEnd;
    float one_by_end_minus_start = end - start;
    one_by_end_minus_start = 1.0f/one_by_end_minus_start;
    _morphConstants.set( end * one_by_end_minus_start, one_by_end_minus_start );

    // Initialize the data model by copying the parent's rendering data
    // and scale/biasing the matrices.
    if (parent)
    {
        unsigned quadrant = getKey().getQuadrant();

        const RenderBindings& bindings = context->getRenderBindings();

        // Copy the parent's rendering model.
        //_renderModel = parent->_renderModel;

        bool setElevation = false;

        for (unsigned p = 0; p < parent->_renderModel._passes.size(); ++p)
        {
            const RenderingPass& parentPass = parent->_renderModel._passes[p];

            if (parentPass.inheritable())
            {
                // Copy the parent pass:
                _renderModel._passes.push_back(parentPass);
                RenderingPass& myPass = _renderModel._passes.back();

                // Scale/bias each matrix for this key quadrant.
                for (unsigned s = 0; s < myPass._samplers.size(); ++s)
                {
                    Sampler& sampler = myPass._samplers[s];
                    sampler._matrix.preMult(scaleBias[quadrant]);
                }

                // Are we using image blending? If so, initialize the color_parent 
                // to the color texture.
                if (bindings[SamplerBinding::COLOR_PARENT].isActive())
                {
                    myPass._samplers[SamplerBinding::COLOR_PARENT] = myPass._samplers[SamplerBinding::COLOR];
                }

                // Use the elevation sampler in the first pass to initialize
                // the elevation raster (used for primitive functors, intersection, etc.)
                if (!setElevation && bindings[SamplerBinding::ELEVATION].isActive())
                {
                    const Sampler& elevation = myPass._samplers[SamplerBinding::ELEVATION];
                    if (elevation._texture.valid())
                    {
                        setElevationRaster(elevation._texture->getImage(0), elevation._matrix);
                        setElevation = true;
                    }
                }
            }
        }
    }

    else
    {
        // If there's no parent, create a default rendering pass with no source.
        // Otherwise we won't get any tiles at all. This will always happen when
        // creating the top-level tiles, so therefore every tile is guaranteed 
        // to have this "default" pass that can be referenced by other layers if
        // need be.
        RenderingPass& defaultPass = _renderModel.addPass();
        defaultPass._sourceUID = -1;
        defaultPass._valid = true;
    }

    // need to recompute the bounds after adding payload:
    dirtyBound();

    // signal the tile to start loading data:
    setDirty( true );

    // register me.
    context->liveTiles()->add( this );
}

osg::BoundingSphere
TileNode::computeBound() const
{
    osg::BoundingSphere bs;
    if (_surface.valid())
    {
        bs = _surface->getBound();
        const osg::BoundingBox bbox = _surface->getAlignedBoundingBox();
        _tileKeyValue.a() = std::max( (bbox.xMax()-bbox.xMin()), (bbox.yMax()-bbox.yMin()) );
    }    
    return bs;
}

bool
TileNode::isDormant(const osg::FrameStamp* fs) const
{
    const unsigned minMinExpiryFrames = 3u;

    bool dormant = 
           fs &&
           fs->getFrameNumber() - _lastTraversalFrame > std::max(_minExpiryFrames, minMinExpiryFrames) &&
           fs->getReferenceTime() - _lastTraversalTime > _minExpiryTime;
    return dormant;
}

void
TileNode::setElevationRaster(const osg::Image* image, const osg::Matrixf& matrix)
{
    if (image == 0L)
    {
        OE_WARN << LC << "TileNode::setElevationRaster: image is NULL!\n";
    }

    if (image != getElevationRaster() || matrix != getElevationMatrix())
    {
        if ( _surface.valid() )
            _surface->setElevationRaster( image, matrix );

        if ( _patch.valid() )
            _patch->setElevationRaster( image, matrix );
    }
}

const osg::Image*
TileNode::getElevationRaster() const
{
    return _surface->getElevationRaster();
}

const osg::Matrixf&
TileNode::getElevationMatrix() const
{
    return _surface->getElevationMatrix();
}

void
TileNode::setDirty(bool value)
{
    _dirty = value;
}

void
TileNode::releaseGLObjects(osg::State* state) const
{
    //OE_WARN << LC << "Tile " << _key.str() << " : Release GL objects\n";

    if ( _surface.valid() )
        _surface->releaseGLObjects(state);

    if ( _patch.valid() )
        _patch->releaseGLObjects(state);

    _renderModel.releaseGLObjects(state);

    osg::Group::releaseGLObjects(state);
}

bool
TileNode::shouldSubDivide(TerrainCuller* culler, const SelectionInfo& selectionInfo)
{    
    unsigned currLOD = _key.getLOD();
    if (currLOD < selectionInfo.numLods() && currLOD != selectionInfo.numLods()-1)
    {
        return _surface->anyChildBoxIntersectsSphere(
            culler->getViewPointLocal(), 
            (float)selectionInfo.visParameters(currLOD+1)._visibilityRange2,
            culler->getLODScale());
    }
    return false;
}

bool
TileNode::cull_stealth(TerrainCuller* culler)
{
    bool visible = false;

    EngineContext* context = culler->getEngineContext();

    // Shows all culled tiles, good for testing culling
    unsigned frame = culler->getFrameStamp()->getFrameNumber();

    if ( frame - _lastAcceptSurfaceFrame < 2u )
    {
        _surface->accept( *culler );
    }

    else if ( _childrenReady )
    {
        for(int i=0; i<4; ++i)
        {
            getSubTile(i)->accept_cull_stealth( culler );
        }
    }

    return visible;
}

bool
TileNode::cull(TerrainCuller* culler)
{
    EngineContext* context = culler->getEngineContext();

    // Horizon check the surface first:
    if (!_surface->isVisibleFrom(culler->getViewPointLocal()))
    {
        return false;
    }
    
    // determine whether we can and should subdivide to a higher resolution:
    bool childrenInRange = shouldSubDivide(culler, context->getSelectionInfo());

    // whether it is OK to create child TileNodes is necessary.
    bool canCreateChildren = childrenInRange;

    // whether it is OK to load data if necessary.
    bool canLoadData = true;

    // whether to accept the current surface node and not the children.
    bool canAcceptSurface = false;
    
    // Don't create children in progressive mode until content is in place
    if ( _dirty && context->getOptions().progressive() == true )
    {
        canCreateChildren = false;
    }
    
    // If this is an inherit-viewpoint camera, we don't need it to invoke subdivision
    // because we want only the tiles loaded by the true viewpoint.
    const osg::Camera* cam = culler->getCamera();
    if ( cam && cam->getReferenceFrame() == osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT )
    {
        canCreateChildren = false;
        canLoadData       = false;
    }

    if (childrenInRange)
    {
        // We are in range of the child nodes. Either draw them or load them.

        // If the children don't exist, create them and inherit the parent's data.
        if ( !_childrenReady && canCreateChildren )
        {
            _mutex.lock();

            if ( !_childrenReady )
            {
                OE_START_TIMER(createChildren);
                createChildren( context );
                REPORT("TileNode::createChildren", createChildren);
                _childrenReady = true;

                // This means that you cannot start loading data immediately; must wait a frame.
                canLoadData = false;
            }

            _mutex.unlock();
        }

        // If all are ready, traverse them now.
        if ( _childrenReady )
        {
            for(int i=0; i<4; ++i)
            {
                getSubTile(i)->accept(*culler);
            }
        }

        // If we don't traverse the children, traverse this node's payload.
        else
        {
            canAcceptSurface = true;
        }
    }

    // If children are outside camera range, draw the payload and expire the children.
    else
    {
        canAcceptSurface = true;
    }

    // accept this surface if necessary.
    if ( canAcceptSurface )
    {
        _surface->accept( *culler );
        _lastAcceptSurfaceFrame.exchange( culler->getFrameStamp()->getFrameNumber() );
    }

       
    // Run any patch callbacks.
    //context->invokeTilePatchCallbacks(cv, getTileKey(), _payloadStateSet.get(), _patch.get() );

    // If this tile is marked dirty, try loading data.
    if ( _dirty && canLoadData )
    {
        load( culler );
    }

    return true;
}

bool
TileNode::accept_cull(TerrainCuller* culler)
{
    bool visible = false;
    
    if (culler)
    {
        // update the timestamp so this tile doesn't become dormant.
        _lastTraversalFrame.exchange( culler->getFrameStamp()->getFrameNumber() );
        _lastTraversalTime = culler->getFrameStamp()->getReferenceTime();

        if ( !culler->isCulled(*this) )
        {
            visible = cull( culler );
        }
    }

    return visible;
}

bool
TileNode::accept_cull_stealth(TerrainCuller* culler)
{
    bool visible = false;
    
    if (culler)
    {
        visible = cull_stealth( culler );
    }

    return visible;
}

void
TileNode::traverse(osg::NodeVisitor& nv)
{
    // Cull only:
    if ( nv.getVisitorType() == nv.CULL_VISITOR )
    {
        TerrainCuller* culler = dynamic_cast<TerrainCuller*>(&nv);
        
        if (VisitorData::isSet(nv, "osgEarth.Stealth"))
        {
            accept_cull_stealth( culler );
        }
        else
        {
            accept_cull( culler );
        }
    }

    // Everything else: update, GL compile, intersection, compute bound, etc.
    else
    {
        // If there are child nodes, traverse them:
        int numChildren = getNumChildren();
        if ( numChildren > 0 )
        {
            for(int i=0; i<numChildren; ++i)
            {
                _children[i]->accept( nv );
            }
        }

        // Otherwise traverse the surface.
        // TODO: in what situations should we traverse the landcover as well? GL compile?
        else 
        {
            _surface->accept( nv );
        }
    }
}

void
TileNode::createChildren(EngineContext* context)
{
    // NOTE: Ensure that _mutex is locked before calling this fucntion!
    //OE_WARN << "Creating children for " << _key.str() << std::endl;

    // Create the four child nodes.
    for(unsigned quadrant=0; quadrant<4; ++quadrant)
    {
        TileNode* node = new TileNode();
        if (context->getOptions().minExpiryFrames().isSet())
        {
            node->setMinimumExpirationFrames( *context->getOptions().minExpiryFrames() );
        }
        if (context->getOptions().minExpiryTime().isSet())
        {         
            node->setMinimumExpirationTime( *context->getOptions().minExpiryTime() );
        }

        // Build the surface geometry:
        node->create( getKey().createChildKey(quadrant), this, context );

        // Add to the scene graph.
        addChild( node );
    }
}

void
TileNode::merge(const TerrainTileModel* model, const RenderBindings& bindings)
{
    // Add color passes:
    const SamplerBinding& color = bindings[SamplerBinding::COLOR];
    if (color.isActive())
    {
        for (TerrainTileImageLayerModelVector::const_iterator i = model->colorLayers().begin();
            i != model->colorLayers().end();
            ++i)
        {
            TerrainTileImageLayerModel* layer = i->get();
            if (layer && layer->getTexture())
            {
                RenderingPass* pass = _renderModel.getPass(layer->getImageLayer()->getUID());
                if (!pass)
                {
                    pass = &_renderModel.addPass();
                    pass->_layer = layer->getImageLayer();
                    pass->_imageLayer = layer->getImageLayer();
                    pass->_sourceUID = layer->getImageLayer()->getUID();
                    pass->_valid = true;

                    // This is a new pass that just showed up at this LOD
                    // Since it just arrived at this LOD, make the parent the same as the color.
                    pass->_samplers[SamplerBinding::COLOR_PARENT]._texture = layer->getTexture();
                    pass->_samplers[SamplerBinding::COLOR_PARENT]._matrix.makeIdentity();
                }
                pass->_samplers[SamplerBinding::COLOR]._texture = layer->getTexture();
                pass->_samplers[SamplerBinding::COLOR]._matrix.makeIdentity();
            }
        }
    }
    //else
    //{
    //    // No color layers? We need a rendering pass with a null texture then
    //    // to accomadate the other samplers.
    //    RenderingPass& pass = _renderModel.addPass();
    //    pass._valid = true;
    //    OE_INFO << LC << "Added default rendering pass..\n";
    //}

    // Elevation:
    const SamplerBinding& elevation = bindings[SamplerBinding::ELEVATION];
    if (elevation.isActive() && model->elevationModel().valid() && model->elevationModel()->getTexture())
    {
        osg::Texture* tex = model->elevationModel()->getTexture();
        // always keep the elevation image around because we use it for bounding box computation:
        tex->setUnRefImageDataAfterApply(false);
        for (unsigned p = 0; p < _renderModel._passes.size(); ++p)
        {
            _renderModel._passes[p]._samplers[SamplerBinding::ELEVATION]._texture = tex;
            _renderModel._passes[p]._samplers[SamplerBinding::ELEVATION]._matrix.makeIdentity();
        }

        setElevationRaster(tex->getImage(0), osg::Matrixf::identity());
    } 

    // Normals:
    const SamplerBinding& normals = bindings[SamplerBinding::NORMAL];
    if (normals.isActive() && model->normalModel().valid() && model->normalModel()->getTexture())
    {
        osg::Texture* tex = model->normalModel()->getTexture();
        // keep the normal map around because we might update it later in "ping"
        tex->setUnRefImageDataAfterApply(false);
        for (unsigned p = 0; p < _renderModel._passes.size(); ++p)
        {
            _renderModel._passes[p]._samplers[SamplerBinding::NORMAL]._texture = tex;
            _renderModel._passes[p]._samplers[SamplerBinding::NORMAL]._matrix.makeIdentity();
        }

        updateNormalMap();
    }

    // Patch Layers
    for (unsigned i = 0; i < model->patchLayers().size(); ++i)
    {
        TerrainTilePatchLayerModel* layerModel = model->patchLayers().at(i);
    }

    // Shared Layers:
    for (unsigned i = 0; i < model->sharedLayers().size(); ++i)
    {
        unsigned bindingIndex = SamplerBinding::SHARED + i;
        const SamplerBinding& binding = bindings[bindingIndex];

        TerrainTileImageLayerModel* layerModel = model->sharedLayers().at(i);
        if (layerModel->getTexture())
        {
            osg::Texture* tex = layerModel->getTexture();
            for (unsigned p = 0; p < _renderModel._passes.size(); ++p)
            {
                _renderModel._passes[p]._samplers[bindingIndex]._texture = tex;
                _renderModel._passes[p]._samplers[bindingIndex]._matrix.makeIdentity();
            }
        }
    }

    if (_childrenReady)
    {
        getSubTile(0)->refreshInheritedData(this, bindings);
        getSubTile(1)->refreshInheritedData(this, bindings);
        getSubTile(2)->refreshInheritedData(this, bindings);
        getSubTile(3)->refreshInheritedData(this, bindings);
    }

    copyCommonSamplers();

#if 0
    // VALIDATION STEP
    for (unsigned p = 0; p < _renderModel._passes.size(); ++p)
    {
        const RenderingPass& pass = _renderModel._passes[p];
        for (unsigned s = SamplerBinding::ELEVATION; s < pass._samplers.size(); ++s)
        {
            for (unsigned k = 0; k < _renderModel._passes.size(); ++k)
            {
                const RenderingPass& kpass = _renderModel._passes[k];
                if (pass._samplers[s]._texture.get() != kpass._samplers[s]._texture.get())
                    OE_WARN << "ERROR: Pass[" << p << "] and Pass[" << k << "] have mismatched texture in sampler[" << s << "]\n";
                if (pass._samplers[s]._matrix != kpass._samplers[s]._matrix)
                    OE_WARN << "ERROR: Pass[" << p << "] and Pass[" << k << "] have mismatched matrix in sampler[" << s << "]\n";
            }
        }
    }
#endif
}

void TileNode::loadChildren()
{
    _mutex.lock();

    if ( !_childrenReady )
    {        
        // Create the children
        createChildren( _context );        
        _childrenReady = true;        
        int numChildren = getNumChildren();
        if ( numChildren > 0 )
        {
            for(int i=0; i<numChildren; ++i)
            {
                TileNode* child = getSubTile(i);
                if (child)
                {
                    // Load the children's data.
                    child->loadSync(_context);
                }
            }
        }
    }
    _mutex.unlock();    
}

void
TileNode::refreshInheritedData(TileNode* parent, const RenderBindings& bindings)
{
    // Run through this tile's rendering data and re-inherit textures and matrixes
    // from the parent. When a TileNode gets new data (via a call to merge), any
    // children of that tile that are inheriting textures or matrixes need to 
    // refresh to inherit that new data. In turn, those tile's children then need
    // to update as well. This method does that.

    // which quadrant is this tile in?
    unsigned quadrant = getKey().getQuadrant();

    // Count the number of inherited samplers so we know when to stop. If none of the
    // samplers in this tile inherit from the parent, there is no need to continue
    // down the Tile tree.
    unsigned changes = 0;

    RenderingPassSet& parentPasses = parent->_renderModel._passes;

    for (unsigned p = 0; p<parentPasses.size(); ++p)
    {
        const RenderingPass& parentPass = parentPasses[p];

        if (parentPass.inheritable())
        {
            RenderingPass* myPass = _renderModel.getPass(parentPass._sourceUID);

            // Inherit the samplers for this pass.
            if (myPass)
            {
                for (unsigned s = 0; s < myPass->_samplers.size(); ++s)
                {
                    Sampler& mySampler = myPass->_samplers[s];
                
                    // the color-parent gets special treatment, since it is not included
                    // in the TileModel (rather it is always derived here).
                    if (s == SamplerBinding::COLOR_PARENT)
                    {
                        const Sampler& parentSampler = parentPass._samplers[SamplerBinding::COLOR];
                        osg::Matrixf newMatrix = parentSampler._matrix;
                        newMatrix.preMult(scaleBias[quadrant]);

                        // Did something change?
                        if (mySampler._texture.get() != parentSampler._texture.get() ||
                            mySampler._matrix != newMatrix)
                        {
                            if (parentSampler._texture.valid())
                            {
                                // set the parent-color texture to the parent's color texture
                                // and scale/bias the matrix.
                                mySampler._texture = parentSampler._texture.get();
                                mySampler._matrix = newMatrix;
                            }
                            else
                            {
                                // parent has no color texture? Then set our parent-color
                                // equal to our normal color texture.
                                mySampler._texture = myPass->_samplers[SamplerBinding::COLOR]._texture.get();
                                mySampler._matrix = myPass->_samplers[SamplerBinding::COLOR]._matrix;
                            }
                            ++changes;
                        }
                    }

                    // all other samplers just need to inherit from their parent 
                    // and scale/bias their texture matrix.
                    else if (!mySampler._texture.valid() || !mySampler._matrix.isIdentity())
                    {
                        const Sampler& parentSampler = parentPass._samplers[s];
                        mySampler._texture = parentSampler._texture.get();
                        mySampler._matrix = parentSampler._matrix;
                        mySampler._matrix.preMult(scaleBias[quadrant]);
                        ++changes;
                    }
                }
            }
            else
            {
                // Pass exists in the parent node, but not in this node, so add it now.
                myPass = &_renderModel.addPass();
                *myPass = parentPass;

                for (unsigned s = 0; s < myPass->_samplers.size(); ++s)
                {
                    Sampler& sampler = myPass->_samplers[s];
                    sampler._matrix.preMult(scaleBias[quadrant]);
                }
                ++changes;
            }
        }
    }

    if (changes > 0)
    {
        dirtyBound(); // only for elev/patch changes maybe?

        copyCommonSamplers();
        
        if (_childrenReady)
        {
            getSubTile(0)->refreshInheritedData(this, bindings);
            getSubTile(1)->refreshInheritedData(this, bindings);
            getSubTile(2)->refreshInheritedData(this, bindings);
            getSubTile(3)->refreshInheritedData(this, bindings);
        }
    }
    else
    {
        //OE_INFO << LC << _key.str() << ": refreshInheritedData, stopped short.\n";
    }
}

void
TileNode::copyCommonSamplers()
{
    const RenderingPass* firstPass = !_renderModel._passes.empty() ? &_renderModel._passes[0] : 0L;
    if (firstPass)
    {
        // Share all "common" samplers, i.e. samplers that are the same across 
        // all rendering passes. This is everything except COLOR and COLOR_PARENT.
        for (unsigned p = 1; p < _renderModel._passes.size(); ++p)
        {
            RenderingPass& pass = _renderModel._passes[p];
            for (unsigned s = SamplerBinding::COLOR_PARENT + 1; s < pass._samplers.size(); ++s)
            {
                pass._samplers[s] = firstPass->_samplers[s];
            }
        }
    }
}

void
TileNode::load(TerrainCuller* culler)
{
    // Access the context:
    EngineContext* context = culler->getEngineContext();

    // Create a new load request on demand:
    if ( !_loadRequest.valid() )
    {
        Threading::ScopedMutexLock lock(_mutex);
        if ( !_loadRequest.valid() )
        {
            _loadRequest = new LoadTileData( this, context );
            _loadRequest->setName( _key.str() );
            _loadRequest->setTileKey( _key );
        }
    }

    
    // Construct the load PRIORITY: 0=lowest, 1=highest.
    
    const SelectionInfo& si = context->getSelectionInfo();
    int lod     = getKey().getLOD();
    int numLods = si.numLods();
    
    // LOD priority is in the range [0..numLods]
    float lodPriority = (float)lod;
    if ( context->getOptions().highResolutionFirst() == false )
        lodPriority = (float)(numLods - lod);

    float distance = culler->getDistanceToViewPoint(getBound().center(), true);

    // dist priority uis in the range [0..1]
    float distPriority = 1.0 - distance/si.visParameters(0)._visibilityRange;

    // add them together, and you get tiles sorted first by lodPriority
    // (because of the biggest range), and second by distance.
    float priority = lodPriority + distPriority;

    // normalize the composite priority to [0..1].
    //priority /= (float)(numLods+1); // GW: moved this to the PagerLoader.

    // Submit to the loader.
    context->getLoader()->load( _loadRequest.get(), priority, *culler );
}

void
TileNode::loadSync(EngineContext* context)
{
    osg::ref_ptr<LoadTileData> loadTileData = new LoadTileData(this, context);
    loadTileData->invoke();
    loadTileData->apply(0L);
}

bool
TileNode::areSubTilesDormant(const osg::FrameStamp* fs) const
{
    return
        getNumChildren() >= 4           &&
        getSubTile(0)->isDormant( fs )  &&
        getSubTile(1)->isDormant( fs )  &&
        getSubTile(2)->isDormant( fs )  &&
        getSubTile(3)->isDormant( fs );
}

void
TileNode::removeSubTiles()
{
    _childrenReady = false;
    this->removeChildren(0, this->getNumChildren());
}


void
TileNode::notifyOfArrival(TileNode* that)
{
    if (_key.createNeighborKey(1, 0) == that->getKey())
        _eastNeighbor = that;

    if (_key.createNeighborKey(0, 1) == that->getKey())
        _southNeighbor = that;

    updateNormalMap();
}

void
TileNode::updateNormalMap()
{
    if ( !_stitchNormalMap )
        return;

    if (_renderModel._passes.empty())
        return;

    RenderingPass& thisPass = _renderModel._passes[0];
    Sampler& thisNormalMap = thisPass._samplers[SamplerBinding::NORMAL];
    if (!thisNormalMap._texture.valid() || !thisNormalMap._matrix.isIdentity() || !thisNormalMap._texture->getImage(0))
        return;

    if (!_eastNeighbor.valid() || !_southNeighbor.valid())
        return;

    osg::ref_ptr<TileNode> east;
    if (_eastNeighbor.lock(east))
    {
        if (east->_renderModel._passes.empty())
            return;

        const RenderingPass& thatPass = east->_renderModel._passes[0];
        const Sampler& thatNormalMap = thatPass._samplers[SamplerBinding::NORMAL];
        if (!thatNormalMap._texture.valid() || !thatNormalMap._matrix.isIdentity() || !thatNormalMap._texture->getImage(0))
            return;

        osg::Image* thisImage = thisNormalMap._texture->getImage(0);
        osg::Image* thatImage = thatNormalMap._texture->getImage(0);

        int width = thisImage->s();
        int height = thisImage->t();
        if ( width != thatImage->s() || height != thatImage->t() )
            return;

        // Just copy the neighbor's edge normals over to our texture.
        // Averaging them would be more accurate, but then we'd have to
        // re-generate each texture multiple times instead of just once.
        // Besides, there's almost no visual difference anyway.
        ImageUtils::PixelReader readThat(thatImage);
        ImageUtils::PixelWriter writeThis(thisImage);
        
        for (int t=0; t<height; ++t)
        {
            writeThis(readThat(0, t), width-1, t);
        }

        thisImage->dirty();
    }

    osg::ref_ptr<TileNode> south;
    if (_southNeighbor.lock(south))
    {
        if (south->_renderModel._passes.empty())
            return;

        const RenderingPass& thatPass = south->_renderModel._passes[0];
        const Sampler& thatNormalMap = thatPass._samplers[SamplerBinding::NORMAL];
        if (!thatNormalMap._texture.valid() || !thatNormalMap._matrix.isIdentity() || !thatNormalMap._texture->getImage(0))
            return;

        osg::Image* thisImage = thisNormalMap._texture->getImage(0);
        osg::Image* thatImage = thatNormalMap._texture->getImage(0);

        int width = thisImage->s();
        int height = thisImage->t();
        if ( width != thatImage->s() || height != thatImage->t() )
            return;

        // Just copy the neighbor's edge normals over to our texture.
        // Averaging them would be more accurate, but then we'd have to
        // re-generate each texture multiple times instead of just once.
        // Besides, there's almost no visual difference anyway.
        ImageUtils::PixelReader readThat(thatImage);
        ImageUtils::PixelWriter writeThis(thisImage);

        for (int s=0; s<width; ++s)
            writeThis(readThat(s, height-1), s, 0);

        thisImage->dirty();
    }

    //OE_INFO << LC << _key.str() << " : updated normal map.\n";
}