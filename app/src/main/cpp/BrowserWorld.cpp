/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserWorld.h"
#include "Widget.h"
#include "WidgetPlacement.h"
#include "vrb/CameraSimple.h"
#include "vrb/Color.h"
#include "vrb/ConcreteClass.h"
#include "vrb/Context.h"
#include "vrb/CullVisitor.h"
#include "vrb/DrawableList.h"
#include "vrb/Geometry.h"
#include "vrb/GLError.h"
#include "vrb/Group.h"
#include "vrb/Light.h"
#include "vrb/Logger.h"
#include "vrb/Matrix.h"
#include "vrb/NodeFactoryObj.h"
#include "vrb/ParserObj.h"
#include "vrb/RenderState.h"
#include "vrb/SurfaceTextureFactory.h"
#include "vrb/TextureCache.h"
#include "vrb/TextureSurface.h"
#include "vrb/Transform.h"
#include "vrb/VertexArray.h"
#include "vrb/Vector.h"
#include <functional>

using namespace vrb;

namespace {

// Must be kept in sync with Widget.java
static const int WidgetTypeBrowser = 0;
static const int WidgetTypeURLBar = 1;

static const int GestureSwipeLeft = 0;
static const int GestureSwipeRight = 1;

static const float kScrollFactor = 20.0f; // Just picked what fell right.
static const float kWorldDPIRatio = 18.0f/720.0f;

static crow::BrowserWorld* sWorld;

static const char* kDispatchCreateWidgetName = "dispatchCreateWidget";
static const char* kDispatchCreateWidgetSignature = "(IILandroid/graphics/SurfaceTexture;III)V";
static const char* kGetDisplayDensityName = "getDisplayDensity";
static const char* kGetDisplayDensitySignature = "()F";
static const char* kHandleMotionEventName = "handleMotionEvent";
static const char* kHandleMotionEventSignature = "(IIZFF)V";
static const char* kHandleScrollEvent = "handleScrollEvent";
static const char* kHandleScrollEventSignature = "(IIFF)V";
static const char* kHandleAudioPoseName = "handleAudioPose";
static const char* kHandleAudioPoseSignature = "(FFFFFFF)V";
static const char* kHandleGestureName = "handleGesture";
static const char* kHandleGestureSignature = "(I)V";
static const char* kTileTexture = "tile.png";
class SurfaceObserver;
typedef std::shared_ptr<SurfaceObserver> SurfaceObserverPtr;

class SurfaceObserver : public SurfaceTextureObserver {
public:
  SurfaceObserver(crow::BrowserWorldWeakPtr& aWorld);
  ~SurfaceObserver();
  void SurfaceTextureCreated(const std::string& aName, GLuint aHandle, jobject aSurfaceTexture) override;
  void SurfaceTextureHandleUpdated(const std::string aName, GLuint aHandle) override;
  void SurfaceTextureDestroyed(const std::string& aName) override;
  void SurfaceTextureCreationError(const std::string& aName, const std::string& aReason) override;

protected:
  crow::BrowserWorldWeakPtr mWorld;
};

SurfaceObserver::SurfaceObserver(crow::BrowserWorldWeakPtr& aWorld) : mWorld(aWorld) {}
SurfaceObserver::~SurfaceObserver() {}

void
SurfaceObserver::SurfaceTextureCreated(const std::string& aName, GLuint aHandle, jobject aSurfaceTexture) {
  crow::BrowserWorldPtr world = mWorld.lock();
  if (world) {
    world->SetSurfaceTexture(aName, aSurfaceTexture);
  }
}

void
SurfaceObserver::SurfaceTextureHandleUpdated(const std::string aName, GLuint aHandle) {}

void
SurfaceObserver::SurfaceTextureDestroyed(const std::string& aName) {
  crow::BrowserWorldPtr world = mWorld.lock();
  if (world) {
    jobject nullObject = nullptr;
    world->SetSurfaceTexture(aName, nullObject);
  }
}

void
SurfaceObserver::SurfaceTextureCreationError(const std::string& aName, const std::string& aReason) {
  
}

struct ControllerRecord {
  int32_t index;
  uint32_t widget;
  bool pressed;
  float xx;
  float yy;
  float touched;
  float touchPadX;
  float touchPadY;
  TransformPtr controller;
  ControllerRecord(const int32_t aIndex) : widget(0), index(aIndex), pressed(false), xx(0.0f), yy(0.0f),
                                           touched(false), touchPadX(0.0f), touchPadY(0.0f) {}
  ControllerRecord(const ControllerRecord& aRecord) : widget(aRecord.widget), index(aRecord.index), controller(aRecord.controller) {}
  ControllerRecord(ControllerRecord&& aRecord) : widget(aRecord.widget), index(aRecord.index), controller(std::move(aRecord.controller)) {}
  void CopyValues(const ControllerRecord& aRecord) {
    index = aRecord.index;
    widget = aRecord.widget;
    pressed = aRecord.pressed;
    xx = aRecord.xx;
    yy = aRecord.yy;
    touched = aRecord.touched;
    touchPadX = aRecord.touchPadX;
    touchPadY = aRecord.touchPadY;
  }
  ControllerRecord& operator=(const ControllerRecord& aRecord) {
    CopyValues(aRecord);
    controller = aRecord.controller;
    return *this;
  }
  ControllerRecord& operator=(ControllerRecord&& aRecord) {
    CopyValues(aRecord);
    controller = std::move(aRecord.controller);
    return *this;
  }
private:
  ControllerRecord() = delete;
};

} // namespace


namespace crow {

struct BrowserWorld::State {
  BrowserWorldWeakPtr self;
  std::vector<WidgetPtr> widgets;
  SurfaceObserverPtr surfaceObserver;
  DeviceDelegatePtr device;
  bool paused;
  bool glInitialized;
  ContextPtr context;
  ContextWeak contextWeak;
  NodeFactoryObjPtr factory;
  ParserObjPtr parser;
  GroupPtr root;
  LightPtr light;
  int32_t controllerCount;
  std::vector<ControllerRecord> controllers;
  CullVisitorPtr cullVisitor;
  DrawableListPtr drawList;
  CameraPtr leftCamera;
  CameraPtr rightCamera;
  float nearClip;
  float farClip;
  JNIEnv* env;
  jobject activity;
  float displayDensity;
  jmethodID dispatchCreateWidgetMethod;
  jmethodID handleMotionEventMethod;
  jmethodID handleScrollEventMethod;
  jmethodID handleAudioPoseMethod;
  jmethodID handleGestureMethod;
  GestureDelegateConstPtr gestures;
  bool windowsInitialized;
  State() : paused(true), glInitialized(false), controllerCount(0), env(nullptr), nearClip(0.1f), farClip(100.0f), activity(nullptr),
            dispatchCreateWidgetMethod(nullptr), handleMotionEventMethod(nullptr), handleScrollEventMethod(nullptr), handleAudioPoseMethod(nullptr), handleGestureMethod(nullptr),
            windowsInitialized(false) {
    context = Context::Create();
    contextWeak = context;
    factory = NodeFactoryObj::Create(contextWeak);
    parser = ParserObj::Create(contextWeak);
    parser->SetObserver(factory);
    root = Group::Create(contextWeak);
    light = Light::Create(contextWeak);
    root->AddLight(light);
    cullVisitor = CullVisitor::Create(contextWeak);
    drawList = DrawableList::Create(contextWeak);
  }

  void InitializeWindows();
  void UpdateControllers();
  WidgetPtr GetWidget(int32_t aHandle) const;
  WidgetPtr FindWidget(const std::function<bool(const WidgetPtr&)>& aCondition) const;
};

void
BrowserWorld::State::InitializeWindows() {
  if (windowsInitialized) {
    return;
  }
  WidgetPtr browser = Widget::Create(contextWeak, WidgetTypeBrowser);
  browser->SetTransform(Matrix::Position(Vector(0.0f, -3.0f, -18.0f)));
  root->AddNode(browser->GetRoot());
  widgets.push_back(std::move(browser));

  WidgetPtr urlbar = Widget::Create(contextWeak, WidgetTypeURLBar,
                                    (int32_t) (720.0f * displayDensity),
                                    (int32_t) (103.0f * displayDensity), 720.0f * kWorldDPIRatio);
  urlbar->SetTransform(Matrix::Position(Vector(0.0f, 7.15f, -18.0f)));
  root->AddNode(urlbar->GetRoot());
  widgets.push_back(std::move(urlbar));
  windowsInitialized = true;
}

void
BrowserWorld::State::UpdateControllers() {
  std::vector<Widget*> active;
  for (ControllerRecord& record: controllers) {
    vrb::Matrix transform = device->GetControllerTransform(record.index);
    record.controller->SetTransform(transform);
    vrb::Vector start = transform.MultiplyPosition(vrb::Vector());
    vrb::Vector direction = transform.MultiplyDirection(vrb::Vector(0.0f, 0.0f, -1.0f));
    WidgetPtr hitWidget;
    float hitDistance = farClip;
    vrb::Vector hitPoint;
    for (const WidgetPtr& widget: widgets) {
      widget->TogglePointer(false);
      vrb::Vector result;
      float distance = 0.0f;
      bool isInWidget = false;
      if (widget->TestControllerIntersection(start, direction, result, isInWidget, distance)) {
        if (isInWidget && (distance < hitDistance)) {
          hitWidget = widget;
          hitDistance = distance;
          hitPoint = result;
        }
      }
    }
    if (gestures) {
      const int32_t gestureCount = gestures->GetGestureCount();
      for (int32_t count = 0; count < gestureCount; count++) {
        const GestureType type = gestures->GetGestureType(count);
        int32_t javaType = -1;
        if (type == GestureType::SwipeLeft) {
          javaType = GestureSwipeLeft;
        } else if (type == GestureType::SwipeRight) {
          javaType = GestureSwipeRight;
        }
        if (javaType >= 0 &&handleGestureMethod) {
         env->CallVoidMethod(activity, handleGestureMethod, javaType);
        }
      }
    }
    if (handleMotionEventMethod && hitWidget) {
      active.push_back(hitWidget.get());
      float theX = 0.0f, theY = 0.0f;
      hitWidget->ConvertToWidgetCoordinates(hitPoint, theX, theY);
      bool changed = false; // not used yet.
      bool pressed = device->GetControllerButtonState(record.index, 0, changed);
      const uint32_t handle = hitWidget->GetHandle();
      if ((record.xx != theX) || (record.yy != theY) || (record.pressed != pressed) || record.widget != handle) {
        env->CallVoidMethod(activity, handleMotionEventMethod, handle, record.index, pressed,
                            theX, theY);
        record.widget = handle;
        record.xx = theX;
        record.yy = theY;
        record.pressed = pressed;
      }
      float scrollX = 0.0f, scrollY = 0.0f;
      if (device->GetControllerScrolled(record.index, scrollX, scrollY)) {
        if (record.touched && !record.pressed) {
          env->CallVoidMethod(activity, handleScrollEventMethod, record.widget, record.index,
                              (scrollX - record.touchPadX) * kScrollFactor, (scrollY - record.touchPadY) * kScrollFactor);
        }
        record.touched = true;
        record.touchPadX = scrollX;
        record.touchPadY = scrollY;
      } else {
        record.touched = false;
      }
    }
  }
  for (Widget* widget: active) {
    widget->TogglePointer(true);
  }
  active.clear();
}

WidgetPtr
BrowserWorld::State::GetWidget(int32_t aHandle) const {
  return FindWidget([=](const WidgetPtr& aWidget){
    return aWidget->GetHandle() == aHandle;
  });
}

WidgetPtr
BrowserWorld::State::FindWidget(const std::function<bool(const WidgetPtr&)>& aCondition) const {
  for (const WidgetPtr & widget: widgets) {
    if (aCondition(widget)) {
      return widget;
    }
  }
  return {};
}

BrowserWorldPtr
BrowserWorld::Create() {
  BrowserWorldPtr result = std::make_shared<vrb::ConcreteClass<BrowserWorld, BrowserWorld::State> >();
  result->m.self = result;
  result->m.surfaceObserver = std::make_shared<SurfaceObserver>(result->m.self);
  result->m.context->GetSurfaceTextureFactory()->AddGlobalObserver(result->m.surfaceObserver);
  return result;
}

vrb::ContextWeak
BrowserWorld::GetWeakContext() {
  return m.context;
}

void
BrowserWorld::RegisterDeviceDelegate(DeviceDelegatePtr aDelegate) {
  m.device = aDelegate;
  if (m.device) {
    m.device->SetClearColor(vrb::Color(0.15f, 0.15f, 0.15f));
    m.leftCamera = m.device->GetCamera(DeviceDelegate::CameraEnum::Left);
    m.rightCamera = m.device->GetCamera(DeviceDelegate::CameraEnum::Right);
    m.controllerCount = m.device->GetControllerCount();
    m.device->SetClipPlanes(m.nearClip, m.farClip);
    m.gestures = m.device->GetGestureDelegate();
  } else {
    m.leftCamera = m.rightCamera = nullptr;
    for (ControllerRecord& record: m.controllers) {
      if (record.controller) {
        m.root->RemoveNode(*record.controller);
      }
    }
    m.controllers.clear();
    m.controllerCount = 0;
    m.gestures = nullptr;
  }
}

void
BrowserWorld::Pause() {
  m.paused = true;
}

void
BrowserWorld::Resume() {
  m.paused = false;
}

bool
BrowserWorld::IsPaused() const {
  return m.paused;
}

void
BrowserWorld::InitializeJava(JNIEnv* aEnv, jobject& aActivity, jobject& aAssetManager) {
  VRB_LOG("BrowserWorld::InitializeJava");
  if (m.context) {
    m.context->InitializeJava(aEnv, aActivity, aAssetManager);
  }
  m.env = aEnv;
  if (!m.env) {
    return;
  }
  m.activity = m.env->NewGlobalRef(aActivity);
  if (!m.activity) {
    return;
  }
  jclass clazz = m.env->GetObjectClass(m.activity);
  if (!clazz) {
    return;
  }

  m.dispatchCreateWidgetMethod = m.env->GetMethodID(clazz, kDispatchCreateWidgetName,
                                                 kDispatchCreateWidgetSignature);
  if (!m.dispatchCreateWidgetMethod) {
    VRB_LOG("Failed to find Java method: %s %s", kDispatchCreateWidgetName, kDispatchCreateWidgetSignature);
  }

  m.handleMotionEventMethod = m.env->GetMethodID(clazz, kHandleMotionEventName, kHandleMotionEventSignature);

  if (!m.handleMotionEventMethod) {
    VRB_LOG("Failed to find Java method: %s %s", kHandleMotionEventName, kHandleMotionEventSignature);
  }

  m.handleScrollEventMethod = m.env->GetMethodID(clazz, kHandleScrollEvent, kHandleScrollEventSignature);

  if (m.handleScrollEventMethod) {
    VRB_LOG("Failed to find Java method: %s %s", kHandleScrollEvent, kHandleScrollEventSignature)
  }

  m.handleAudioPoseMethod =  m.env->GetMethodID(clazz, kHandleAudioPoseName, kHandleAudioPoseSignature);

  if (!m.handleAudioPoseMethod) {
    VRB_LOG("Failed to find Java method: %s %s", kHandleAudioPoseName, kHandleAudioPoseSignature);
  }

  m.handleGestureMethod = m.env->GetMethodID(clazz, kHandleGestureName, kHandleGestureSignature);

  if (!m.handleGestureMethod) {
    VRB_LOG("Failed to find Java method: %s %s", kHandleGestureName, kHandleGestureSignature);
  }

  jmethodID getDisplayDensityMethod =  m.env->GetMethodID(clazz, kGetDisplayDensityName, kGetDisplayDensitySignature);
  if (getDisplayDensityMethod) {
    m.displayDensity = m.env->CallFloatMethod(m.activity, getDisplayDensityMethod);
  }

  m.InitializeWindows();

  if ((m.controllers.size() == 0) && (m.controllerCount > 0)) {
    for (int32_t ix = 0; ix < m.controllerCount; ix++) {
      ControllerRecord record(ix);
      record.controller = Transform::Create(m.contextWeak);
      const std::string fileName = m.device->GetControllerModelName(ix);
      if (!fileName.empty()) {
        m.factory->SetModelRoot(record.controller);
        m.parser->LoadModel(m.device->GetControllerModelName(ix));
        m.root->AddNode(record.controller);
      }
      m.controllers.push_back(std::move(record));
    }
    AddControllerPointer();
    CreateFloor();
  }
}

void
BrowserWorld::InitializeGL() {
  VRB_LOG("BrowserWorld::InitializeGL");
  if (m.context) {
    if (!m.glInitialized) {
      m.glInitialized = m.context->InitializeGL();
      VRB_CHECK(glEnable(GL_BLEND));
      VRB_CHECK(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
      if (!m.glInitialized) {
        return;
      }
      SurfaceTextureFactoryPtr factory = m.context->GetSurfaceTextureFactory();
      for (WidgetPtr& widget: m.widgets) {
        const std::string name = widget->GetSurfaceTextureName();
        jobject surface = factory->LookupSurfaceTexture(name);
        if (surface) {
          SetSurfaceTexture(name, surface);
        }
      }
    }
  }
}

void
BrowserWorld::ShutdownJava() {
  VRB_LOG("BrowserWorld::ShutdownJava");
  if (m.env) {
    m.env->DeleteGlobalRef(m.activity);
  }
  m.activity = nullptr;
  m.dispatchCreateWidgetMethod = nullptr;
  m.handleMotionEventMethod = nullptr;
  m.handleScrollEventMethod = nullptr;
  m.handleAudioPoseMethod = nullptr;
  m.handleGestureMethod = nullptr;
  m.env = nullptr;
}

void
BrowserWorld::ShutdownGL() {
  VRB_LOG("BrowserWorld::ShutdownGL");
  if (m.context) {
    m.context->ShutdownGL();
  }
  m.glInitialized = false;
}

void
BrowserWorld::Draw() {
  if (!m.device) {
    VRB_LOG("No device");
    return;
  }
  if (m.paused) {
    VRB_LOG("BrowserWorld Paused");
    return;
  }
  if (!m.glInitialized) {
    m.glInitialized = m.context->InitializeGL();
    if (!m.glInitialized) {
      VRB_LOG("FAILED to initialize GL");
      return;
    }
  }
  m.device->ProcessEvents();
  m.context->Update();
  m.UpdateControllers();
  m.drawList->Reset();
  m.root->Cull(*m.cullVisitor, *m.drawList);
  m.device->StartFrame();
  m.device->BindEye(DeviceDelegate::CameraEnum::Left);
  m.drawList->Draw(*m.leftCamera);
  // When running the noapi flavor, we only want to render one eye.
#if !defined(VRBROWSER_NO_VR_API)
  m.device->BindEye(DeviceDelegate::CameraEnum::Right);
  m.drawList->Draw(*m.rightCamera);
#endif // !defined(VRBROWSER_NO_VR_API)
  m.device->EndFrame();

  // Update the 3d audio engine with the most recent head rotation.
  if (m.handleAudioPoseMethod) {
    const vrb::Matrix &head = m.device->GetHeadTransform();
    const vrb::Vector p = head.GetTranslation();
    const vrb::Quaternion q(head);
    m.env->CallVoidMethod(m.activity, m.handleAudioPoseMethod, q.x(), q.y(), q.z(), q.w(), p.x(), p.y(), p.z());
  }

}

void
BrowserWorld::SetSurfaceTexture(const std::string& aName, jobject& aSurface) {
  VRB_LOG("SetSurfaceTexture: %s", aName.c_str());
  if (m.env && m.activity && m.dispatchCreateWidgetMethod) {
    WidgetPtr widget = m.FindWidget([=](const WidgetPtr& aWidget) {
      return aName == aWidget->GetSurfaceTextureName();
    });
    if (widget) {
      int32_t width = 0, height = 0;
      widget->GetSurfaceTextureSize(width, height);
      int32_t callbackId = widget->GetAddCallbackId();
      m.env->CallVoidMethod(m.activity, m.dispatchCreateWidgetMethod, widget->GetType(),
                            widget->GetHandle(), aSurface, width, height, callbackId);
    }
  }
}

void
BrowserWorld::AddWidget(const WidgetPlacement& placement, int32_t aCallbackId) {
  WidgetPtr parent = m.GetWidget(placement.parentHandle);
  if (!parent) {
    VRB_LOG("Can't find Widget with handle: %d", placement.parentHandle);
    return;
  }

  int32_t parentWidth, parentHeight;
  float parentWorldWith, parentWorldHeight;
  parent->GetSurfaceTextureSize(parentWidth, parentHeight);
  parent->GetWorldSize(parentWorldWith, parentWorldHeight);

  float worldWidth = placement.width * kWorldDPIRatio;
  float worldHeight;

  WidgetPtr widget = Widget::Create(m.contextWeak,
                                    placement.widgetType,
                                    placement.width * m.displayDensity,
                                    placement.height * m.displayDensity,
                                    worldWidth);
  widget->SetAddCallbackId(aCallbackId);
  widget->GetWorldSize(worldWidth, worldHeight);

  vrb::Vector translation = vrb::Vector(placement.translation.x() * kWorldDPIRatio,
                                        placement.translation.y() * kWorldDPIRatio,
                                        placement.translation.z() * kWorldDPIRatio);
  // Widget anchor point
  translation -= vrb::Vector((placement.anchor.x() - 0.5f) * worldWidth,
                             placement.anchor.y() * worldHeight,
                             0.0f);
  // Parent anchor point
  translation += vrb::Vector(parentWorldWith * placement.parentAnchor.x() - parentWorldWith * 0.5f,
                             parentWorldHeight * placement.parentAnchor.y(),
                             0.0f);

  widget->SetTransform(parent->GetTransform().PostMultiply(vrb::Matrix::Translation(translation)));
  m.root->AddNode(widget->GetRoot());
  m.widgets.push_back(widget);
}

void
BrowserWorld::SetWidgetVisible(int32_t aHandle, bool aVisible) {
  WidgetPtr widget = m.GetWidget(aHandle);
  if (widget) {
    widget->ToggleWidget(aVisible);
  }
}

void
BrowserWorld::RemoveWidget(int32_t aHandle) {
  WidgetPtr widget = m.GetWidget(aHandle);
  if (widget) {
    widget->GetRoot()->RemoveFromParents();
    auto it = std::find(m.widgets.begin(), m.widgets.end(), widget);
    if (it != m.widgets.end()) {
      m.widgets.erase(it);
    }
  }
}

JNIEnv*
BrowserWorld::GetJNIEnv() const {
  return m.env;
}

BrowserWorld::BrowserWorld(State& aState) : m(aState) {
  sWorld = this;
}

BrowserWorld::~BrowserWorld() {
 if (sWorld == this) {
  sWorld = nullptr;
 }
}

void
BrowserWorld::CreateFloor() {
  VertexArrayPtr array = VertexArray::Create(m.contextWeak);
  const float kLength = 5.0f;
  const float kFloor = 0.0f;
  array->AppendVertex(Vector(-kLength, kFloor, kLength)); // Bottom left
  array->AppendVertex(Vector(kLength, kFloor, kLength)); // Bottom right
  array->AppendVertex(Vector(kLength, kFloor, -kLength)); // Top right
  array->AppendVertex(Vector(-kLength, kFloor, -kLength)); // Top left

  const float kUV = kLength * 2.0f;
  array->AppendUV(Vector(0.0f, 0.0f, 0.0f));
  array->AppendUV(Vector(kUV, 0.0f, 0.0f));
  array->AppendUV(Vector(kUV, kUV, 0.0f));
  array->AppendUV(Vector(0.0f, kUV, 0.0f));

  const Vector kNormal(0.0f, 1.0f, 0.0f);
  array->AppendNormal(kNormal);

  RenderStatePtr state = RenderState::Create(m.contextWeak);
  TexturePtr tile = m.context->GetTextureCache()->LoadTexture(kTileTexture);
  if (tile) {
    tile->SetTextureParameter(GL_TEXTURE_WRAP_S, GL_REPEAT);
    tile->SetTextureParameter(GL_TEXTURE_WRAP_T, GL_REPEAT);
    state->SetTexture(tile);
  }
  state->SetMaterial(Color(0.4f, 0.4f, 0.4f), Color(1.0f, 1.0f, 1.0f), Color(0.0f, 0.0f, 0.0f),
                     0.0f);
  GeometryPtr geometry = Geometry::Create(m.contextWeak);
  geometry->SetVertexArray(array);
  geometry->SetRenderState(state);

  std::vector<int> index;
  index.push_back(1);
  index.push_back(2);
  index.push_back(3);
  index.push_back(4);
  std::vector<int> normalIndex;
  normalIndex.push_back(1);
  normalIndex.push_back(1);
  normalIndex.push_back(1);
  normalIndex.push_back(1);
  geometry->AddFace(index, index, normalIndex);

  m.root->AddNode(geometry);
}

void
BrowserWorld::AddControllerPointer() {
  VertexArrayPtr array = VertexArray::Create(m.contextWeak);
  const float kLength = -5.0f;
  const float kHeight = 0.0008f;

  array->AppendVertex(Vector(-kHeight, -kHeight, 0.0f)); // Bottom left
  array->AppendVertex(Vector(kHeight, -kHeight, 0.0f)); // Bottom right
  array->AppendVertex(Vector(kHeight, kHeight, 0.0f)); // Top right
  array->AppendVertex(Vector(-kHeight, kHeight, 0.0f)); // Top left
  array->AppendVertex(Vector(0.0f, 0.0f, kLength)); // Tip

  array->AppendNormal(Vector(-1.0f, -1.0f, 0.0f).Normalize()); // Bottom left
  array->AppendNormal(Vector(1.0f, -1.0f, 0.0f).Normalize()); // Bottom right
  array->AppendNormal(Vector(1.0f, 1.0f, 0.0f).Normalize()); // Top right
  array->AppendNormal(Vector(-1.0f, 1.0f, 0.0f).Normalize()); // Top left
  array->AppendNormal(Vector(0.0f, 0.0f, -1.0f).Normalize()); // in to the screen


  RenderStatePtr state = RenderState::Create(m.contextWeak);
  state->SetMaterial(Color(0.6f, 0.0f, 0.0f), Color(1.0f, 0.0f, 0.0f), Color(0.5f, 0.5f, 0.5f),
                     96.078431);
  GeometryPtr geometry = Geometry::Create(m.contextWeak);
  geometry->SetVertexArray(array);
  geometry->SetRenderState(state);

  std::vector<int> index;
  std::vector<int> uvIndex;

  index.push_back(1);
  index.push_back(2);
  index.push_back(5);
  geometry->AddFace(index, uvIndex, index);

  index.clear();
  index.push_back(2);
  index.push_back(3);
  index.push_back(5);
  geometry->AddFace(index, uvIndex, index);

  index.clear();
  index.push_back(3);
  index.push_back(4);
  index.push_back(5);
  geometry->AddFace(index, uvIndex, index);

  index.clear();
  index.push_back(4);
  index.push_back(1);
  index.push_back(5);
  geometry->AddFace(index, uvIndex, index);

  for (ControllerRecord& record: m.controllers) {
    record.controller->AddNode(geometry);
  }
}

} // namespace crow


#define JNI_METHOD(return_type, method_name) \
  JNIEXPORT return_type JNICALL              \
    Java_org_mozilla_vrbrowser_VRBrowserActivity_##method_name

extern "C" {

JNI_METHOD(void, addWidgetNative)
(JNIEnv*, jobject thiz, jobject data, jint aCallbackId) {
  crow::WidgetPlacementPtr placement = crow::WidgetPlacement::FromJava(sWorld->GetJNIEnv(), data);
  if (placement && sWorld) {
    sWorld->AddWidget(*placement, aCallbackId);
  }
}

JNI_METHOD(void, setWidgetVisibleNative)
(JNIEnv*, jobject thiz, jint aHandle, jboolean aVisible) {
  if (sWorld) {
    sWorld->SetWidgetVisible(aHandle, aVisible);
  }
}

JNI_METHOD(void, removeWidgetNative)
(JNIEnv*, jobject thiz, jint aHandle) {
  if (sWorld) {
    sWorld->RemoveWidget(aHandle);
  }
}

} // extern "C"
