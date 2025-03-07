// acmodel.cxx - manage a 3D aircraft model.
// Written by David Megginson, started 2002.
//
// This file is in the Public Domain, and comes with no warranty.

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <cstring>		// for strcmp()

#include <simgear/compiler.h>
#include <simgear/debug/ErrorReportingCallback.hxx>
#include <simgear/debug/logstream.hxx>

#include <simgear/structure/exception.hxx>
#include <simgear/misc/sg_path.hxx>
#include <simgear/scene/model/placement.hxx>
#include <simgear/scene/util/SGNodeMasks.hxx>
#include <simgear/scene/model/modellib.hxx>

#include <Main/globals.hxx>
#include <Main/fg_props.hxx>
#include <Viewer/renderer.hxx>
#include <Viewer/viewmgr.hxx>
#include <Viewer/view.hxx>
#include <Scenery/scenery.hxx>
#include <Sound/fg_fx.hxx>

#include "acmodel.hxx"


static osg::Node *
fgLoad3DModelPanel(const SGPath &path, SGPropertyNode *prop_root)
{
    bool loadPanels = true;
    bool autoTooltipsMaster = fgGetBool("/sim/rendering/automatic-animation-tooltips/enabled");
    int autoTooltipsMasterMax = fgGetInt("/sim/rendering/automatic-animation-tooltips/max-count");
    SG_LOG(SG_INPUT, SG_DEBUG, ""
            << " autoTooltipsMaster=" << autoTooltipsMaster
            << " autoTooltipsMasterMax=" << autoTooltipsMasterMax
            );
    osg::Node* node = simgear::SGModelLib::loadModel(path.utf8Str(), prop_root, NULL, loadPanels, autoTooltipsMaster, autoTooltipsMasterMax);
    if (node)
        node->setNodeMask(~SG_NODEMASK_TERRAIN_BIT);
    return node;
}

////////////////////////////////////////////////////////////////////////
// Implementation of FGAircraftModel
////////////////////////////////////////////////////////////////////////

FGAircraftModel::FGAircraftModel ()
  : _velocity(SGVec3d::zeros()),
    _fx(0),
    _speed_n(0),
    _speed_e(0),
    _speed_d(0)
{
}

FGAircraftModel::~FGAircraftModel ()
{
  // drop reference
  _fx = 0;
  shutdown();
}

void
FGAircraftModel::init ()
{
    if (_aircraft.get()) {
        SG_LOG(SG_AIRCRAFT, SG_ALERT, "FGAircraftModel::init: already inited");
        return;
    }

    _fx = new FGFX("fx");
    _fx->init();
    simgear::ErrorReportContext ec("primary-aircraft", "yes");

    SGPropertyNode_ptr sim = fgGetNode("/sim", true);
    for (auto model : sim->getChildren("model")) {
        std::string path = model->getStringValue("path", "Models/Geometry/glider.ac");
        std::string usage = model->getStringValue("usage", "external");

        simgear::ErrorReportContext ec("aircraft-model", path);

        SGPath resolvedPath = globals->resolve_aircraft_path(path);
        if (resolvedPath.isNull()) {
            simgear::reportFailure(simgear::LoadFailure::NotFound,
                                   simgear::ErrorCode::XMLModelLoad,
                                   "Failed to find aircraft model", SGPath::fromUtf8(path));
            SG_LOG(SG_AIRCRAFT, SG_ALERT, "Failed to find aircraft model: " << path);
            continue;
        }

        osg::Node* node = NULL;
        try {
            node = fgLoad3DModelPanel( resolvedPath, globals->get_props());
        } catch (const sg_exception &ex) {
            simgear::reportFailure(simgear::LoadFailure::BadData,
                                   simgear::ErrorCode::XMLModelLoad,
                                   "Failed to load aircraft model:" + ex.getFormattedMessage(),
                                   ex.getLocation());
            SG_LOG(SG_AIRCRAFT, SG_ALERT, "Failed to load aircraft from " << path << ':');
            SG_LOG(SG_AIRCRAFT, SG_ALERT, "  " << ex.getFormattedMessage());
        }

        if (usage == "interior") {
            // interior model
            if (!_interior.get()) {
                _interior.reset(new SGModelPlacement);
                _interior->init(node);
            } else {
                _interior->add(node);
            }
        } else {
            // normal / exterior model
            if (!_aircraft.get()) {
                _aircraft.reset(new SGModelPlacement);
                _aircraft->init(node);
            } else {
                _aircraft->add(node);
            }
        } // of model usage switch
    } // of models iteration

    // no models loaded, load the glider instead
    if (!_aircraft.get()) {
        SG_LOG(SG_AIRCRAFT, SG_ALERT, "(Falling back to glider.ac.)");
        osg::Node* model = fgLoad3DModelPanel( SGPath::fromUtf8("Models/Geometry/glider.ac"),
                                   globals->get_props());
        _aircraft.reset(new SGModelPlacement);
        _aircraft->init(model);

    }

  osg::Node* node = _aircraft->getSceneGraph();
  globals->get_scenery()->get_aircraft_branch()->addChild(node);

    if (_interior.get()) {
        node = _interior->getSceneGraph();
        globals->get_scenery()->get_interior_branch()->addChild(node);
    }
}

void
FGAircraftModel::reinit()
{
  shutdown();
  _fx->reinit();
  init();
  // TODO globally create signals for all subsystems (re)initialized
  fgSetBool("/sim/signals/model-reinit", true);
}

void
FGAircraftModel::shutdown()
{
    FGScenery* scenery = globals->get_scenery();

    if (_aircraft.get()) {
        if (scenery && scenery->get_aircraft_branch()) {
            scenery->get_aircraft_branch()->removeChild(_aircraft->getSceneGraph());
        }
    }

    if (_interior.get()) {
        if (scenery && scenery->get_interior_branch()) {
            scenery->get_interior_branch()->removeChild(_interior->getSceneGraph());
        }
    }

    _aircraft.reset();
    _interior.reset();
}

void
FGAircraftModel::bind ()
{
   _speed_n = fgGetNode("velocities/speed-north-fps", true);
   _speed_e = fgGetNode("velocities/speed-east-fps", true);
   _speed_d = fgGetNode("velocities/speed-down-fps", true);
}

void
FGAircraftModel::unbind ()
{
  _fx->unbind();
}

void
FGAircraftModel::update (double dt)
{
    int view_number = globals->get_viewmgr()->getCurrentViewIndex();
    int is_internal = fgGetBool("/sim/current-view/internal");

    if (view_number == 0 && !is_internal) {
        _aircraft->setVisible(false);
  } else {
    _aircraft->setVisible(true);
  }

    double heading, pitch, roll;
    globals->get_aircraft_orientation(heading, pitch, roll);
    SGQuatd orient = SGQuatd::fromYawPitchRollDeg(heading, pitch, roll);

    SGGeod pos = globals->get_aircraft_position();

    _aircraft->setPosition(pos);
    _aircraft->setOrientation(orient);
    _aircraft->update();

    if (_interior.get()) {
        _interior->setPosition(pos);
        _interior->setOrientation(orient);
        _interior->update();
    }

  // update model's audio sample values
  _fx->set_position_geod( pos );
  _fx->set_orientation( orient );

  _velocity = SGVec3d( _speed_n->getDoubleValue(),
                       _speed_e->getDoubleValue(),
                       _speed_d->getDoubleValue() );
  _fx->set_velocity( _velocity );

  float temp_c = fgGetFloat("/environment/temperature-degc");
  float humidity = fgGetFloat("/environment/relative-humidity");
  float pressure = fgGetFloat("/environment/pressure-inhg")*SG_INHG_TO_PA/1000.0f;
  _fx->set_atmosphere( temp_c, humidity, pressure );
}


// Register the subsystem.
SGSubsystemMgr::Registrant<FGAircraftModel> registrantFGAircraftModel(
    SGSubsystemMgr::DISPLAY);

// end of model.cxx
