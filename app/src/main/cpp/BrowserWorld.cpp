/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserWorld.h"
#include "ControllerDelegate.h"
#include "Tray.h"
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
#include "vrb/Toggle.h"
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
static const char* kHandleTrayEventName = "handleTrayEvent";
static const char* kHandleTrayEventSignature = "(I)V";
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
SurfaceObserver::SurfaceTextureCreated(const std::string& aName, GLuint, jobject aSurfaceTexture) {
  crow::BrowserWorldPtr world = mWorld.lock();
  if (world) {
    world->SetSurfaceTexture(aName, aSurfaceTexture);
  }
}

void
SurfaceObserver::SurfaceTextureHandleUpdated(const std::string, GLuint) {}

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

struct Controller {
  int32_t index;
  bool enabled;
  uint32_t widget;
  float pointerX;
  float pointerY;
  bool pressed;
  bool wasPressed;
  bool touched;
  bool wasTouched;
  float touchX;
  float touchY;
  float lastTouchX;
  float lastTouchY;
  float scrollDeltaX;
  float scrollDeltaY;
  TransformPtr transform;
  Matrix transformMatrix;
  
  Controller() : index(-1), enabled(false), widget(0),
                 pointerX(0.0f), pointerY(0.0f),
                 pressed(false), wasPressed(false),
                 touched(false), wasTouched(false),
                 touchX(0.0f), touchY(0.0f),
                 lastTouchX(0.0f), lastTouchY(0.0f),
                 scrollDeltaX(0.0f), scrollDeltaY(0.0f),
                 transformMatrix(Matrix::Identity()) {}

  Controller(const Controller& aController) {
    *this = aController;
  }

  ~Controller() {
    Reset();
  }

  Controller& operator=(const Controller& aController) {
    index = aController.index;
    enabled = aController.enabled;
    widget = aController.widget;
    pointerX = aController.pointerX;
    pointerY = aController.pointerY;
    pressed = aController.pressed;
    wasPressed = aController.wasPressed;
    touched = aController.touched;
    wasTouched = aController.wasTouched;
    touchX = aController.touchX;
    touchY= aController.touchY;
    lastTouchX = aController.lastTouchX;
    lastTouchY = aController.lastTouchY;
    scrollDeltaX = aController.scrollDeltaX;
    scrollDeltaY = aController.scrollDeltaY;
    transform = aController.transform;
    transformMatrix = aController.transformMatrix;
    return *this;
  }

  void Reset() {
    index = -1;
    enabled = false;
    widget = 0;
    pointerX = pointerY = 0.0f;
    pressed = wasPressed = false;
    touched = wasTouched = false;
    touchX = touchY = 0.0f;
    lastTouchX = lastTouchY = 0.0f;
    scrollDeltaX = scrollDeltaY = 0.0f;
    if (transform) {
      transform = nullptr;
    }
    transformMatrix = Matrix::Identity();
  }
};

class ControllerContainer;
typedef std::shared_ptr<ControllerContainer> ControllerContainerPtr;

class ControllerContainer : public crow::ControllerDelegate {
public:
  static ControllerContainerPtr Create();
  ControllerContainer() : modelsLoaded(false) {}
  ~ControllerContainer();
  void SetUpModelsGroup(const int32_t aModelIndex);
  // crow::ControllerDelegate interface
  void CreateController(const int32_t aControllerIndex, const int32_t aModelIndex) override;
  void DestroyController(const int32_t aControllerIndex) override;
  void SetEnabled(const int32_t aControllerIndex, const bool aEnabled) override;
  void SetVisible(const int32_t aControllerIndex, const bool aVisible) override;
  void SetTransform(const int32_t aControllerIndex, const vrb::Matrix& aTransform) override;
  void SetButtonState(const int32_t aControllerIndex, const int32_t aWhichButton, const bool aPressed) override;
  void SetTouchPosition(const int32_t aControllerIndex, const float aTouchX, const float aTouchY) override;
  void EndTouch(const int32_t aControllerIndex) override;
  void SetScrolledDelta(const int32_t aControllerIndex, const float aScrollDeltaX, const float aScrollDeltaY) override;
  std::vector<Controller> list;
  ContextWeak context;
  TogglePtr root;
  bool modelsLoaded;
  std::vector<GroupPtr> models;
  GeometryPtr pointerModel;
  bool Contains(const int32_t aControllerIndex) {
    return (aControllerIndex >= 0) && (aControllerIndex < list.size());
  }
};

ControllerContainerPtr
ControllerContainer::Create() {
  return std::make_shared<ControllerContainer>();
}

ControllerContainer::~ControllerContainer() {
  if (root) {
    root->RemoveFromParents();
    root = nullptr;
  }
}

void
ControllerContainer::SetUpModelsGroup(const int32_t aModelIndex) {
  if (models.size() >= aModelIndex) {
    models.resize((size_t)(aModelIndex + 1));
  }
  if (!models[aModelIndex]) {
    models[aModelIndex] = std::move(Group::Create(context));
  }
}

void
ControllerContainer::CreateController(const int32_t aControllerIndex, const int32_t aModelIndex) {
  if ((size_t)aControllerIndex >= list.size()) {
    list.resize((size_t)aControllerIndex + 1);
  }
  Controller& controller = list[aControllerIndex];
  controller.index = aControllerIndex;
  if (!controller.transform && (aModelIndex >= 0)) {
    SetUpModelsGroup(aModelIndex);
    controller.transform = Transform::Create(context);
    if ((models.size() >= aModelIndex) && models[aModelIndex]) {
      controller.transform->AddNode(models[aModelIndex]);
      if (pointerModel) {
        controller.transform->AddNode(pointerModel);
      }
      if (root) {
        root->AddNode(controller.transform);
        root->ToggleChild(*controller.transform, false);
      }
    } else {
      VRB_LOG("FAILED TO ADD MODEL");
    }
  }
}

void
ControllerContainer::DestroyController(const int32_t aControllerIndex) {
  if (Contains(aControllerIndex)) {
    list[aControllerIndex].Reset();
  }
}

void
ControllerContainer::SetEnabled(const int32_t aControllerIndex, const bool aEnabled) {
  if (!Contains(aControllerIndex)) {
    return;
  }
  list[aControllerIndex].enabled = aEnabled;
  if (!aEnabled) {
    SetVisible(aControllerIndex, false);
  }
}

void
ControllerContainer::SetVisible(const int32_t aControllerIndex, const bool aVisible) {
  if (!Contains(aControllerIndex)) {
    return;
  }
  Controller& controller = list[aControllerIndex];
  if (controller.transform) {
    root->ToggleChild(*controller.transform, aVisible);
  }
}

void
ControllerContainer::SetTransform(const int32_t aControllerIndex, const vrb::Matrix& aTransform) {
  if (!Contains(aControllerIndex)) {
    return;
  }
  Controller& controller = list[aControllerIndex];
  controller.transformMatrix = aTransform;
  if (controller.transform) {
    controller.transform->SetTransform(aTransform);
  }
}

void
ControllerContainer::SetButtonState(const int32_t aControllerIndex, const int32_t aWhichButton, const bool aPressed) {
  if (!Contains(aControllerIndex)) {
    return;
  }
  list[aControllerIndex].pressed = aPressed;
}

void
ControllerContainer::SetTouchPosition(const int32_t aControllerIndex, const float aTouchX, const float aTouchY) {
  if (!Contains(aControllerIndex)) {
    return;
  }
  Controller& controller = list[aControllerIndex];
  controller.touched = true;
  controller.touchX = aTouchX;
  controller.touchY = aTouchY;
}

void
ControllerContainer::EndTouch(const int32_t aControllerIndex) {
  if (!Contains(aControllerIndex)) {
    return;
  }
  list[aControllerIndex].touched = false;
}

void
ControllerContainer::SetScrolledDelta(const int32_t aControllerIndex, const float aScrollDeltaX, const float aScrollDeltaY) {
  if (!Contains(aControllerIndex)) {
    return;
  }
  Controller& controller = list[aControllerIndex];
  controller.scrollDeltaX = aScrollDeltaX;
  controller.scrollDeltaY = aScrollDeltaY;
}

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
  ControllerContainerPtr controllers;
  CullVisitorPtr cullVisitor;
  DrawableListPtr drawList;
  CameraPtr leftCamera;
  CameraPtr rightCamera;
  TrayPtr tray;
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
  jmethodID handleTrayEventMethod;
  GestureDelegateConstPtr gestures;
  bool windowsInitialized;

  State() : paused(true), glInitialized(false), env(nullptr), nearClip(0.1f),
            farClip(100.0f), activity(nullptr),
            dispatchCreateWidgetMethod(nullptr), handleMotionEventMethod(nullptr),
            handleScrollEventMethod(nullptr), handleAudioPoseMethod(nullptr),
            handleGestureMethod(nullptr),
            handleTrayEventMethod(nullptr),
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
    controllers = ControllerContainer::Create();
    controllers->context = contextWeak;
    controllers->root = Toggle::Create(contextWeak);
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
  for (Controller& controller: controllers->list) {
    if (!controller.enabled || (controller.index < 0)) {
      continue;
    }
    vrb::Vector start = controller.transformMatrix.MultiplyPosition(vrb::Vector());
    vrb::Vector direction = controller.transformMatrix.MultiplyDirection(vrb::Vector(0.0f, 0.0f, -1.0f));
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

    if (tray) {
      vrb::Vector result;
      float distance = 0.0f;
      bool isInside = false;
      bool trayActive = false;
      if (tray->TestControllerIntersection(start, direction, result, isInside, distance)) {
        if (isInside && (distance < hitDistance)) {
          hitWidget.reset();
          hitDistance = distance;
          hitPoint = result;
          trayActive = true;
        }
      }
      int32_t trayEvent = tray->ProcessEvents(trayActive, controller.pressed);
      if (trayEvent == Tray::IconHide) {
        tray->Toggle(false);
      }
      if (trayEvent >= 0 && handleTrayEventMethod) {
        env->CallVoidMethod(activity, handleTrayEventMethod, trayEvent);
      }
    }

    if (handleMotionEventMethod && hitWidget) {
      active.push_back(hitWidget.get());
      float theX = 0.0f, theY = 0.0f;
      hitWidget->ConvertToWidgetCoordinates(hitPoint, theX, theY);
      const uint32_t handle = hitWidget->GetHandle();
      if ((controller.pointerX != theX) ||
          (controller.pointerY != theY) ||
          (controller.pressed != controller.wasPressed) || controller.widget != handle) {
        env->CallVoidMethod(activity, handleMotionEventMethod, handle, controller.index,
                            controller.pressed,
                            theX, theY);
        controller.widget = handle;
        controller.pointerX = theX;
        controller.pointerY = theY;
        controller.wasPressed = controller.pressed;
      }
      if ((controller.scrollDeltaX != 0.0f) || controller.scrollDeltaY != 0.0f) {
        env->CallVoidMethod(activity, handleScrollEventMethod, controller.widget, controller.index,
                            controller.scrollDeltaX, controller.scrollDeltaY);
        controller.scrollDeltaX = 0.0f;
        controller.scrollDeltaY = 0.0f;
      }
      if (!controller.pressed) {
        if (controller.touched) {
          if (!controller.wasTouched) {
            controller.wasTouched = controller.touched;
          } else {
            env->CallVoidMethod(activity, handleScrollEventMethod, controller.widget,
                                controller.index,
                                (controller.touchX - controller.lastTouchX) * kScrollFactor,
                                (controller.touchY - controller.lastTouchY) * kScrollFactor);
          }
          controller.lastTouchX = controller.touchX;
          controller.lastTouchY = controller.touchY;
        } else {
          controller.wasTouched = false;
          controller.lastTouchX = controller.lastTouchY = 0.0f;
        }
      }
    }
  }
  for (Widget* widget: active) {
    widget->TogglePointer(true);
  }
  active.clear();
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
  DeviceDelegatePtr previousDevice = std::move(m.device);
  m.device = aDelegate;
  if (m.device) {
    m.device->SetClearColor(vrb::Color(0.15f, 0.15f, 0.15f));
    m.leftCamera = m.device->GetCamera(DeviceDelegate::CameraEnum::Left);
    m.rightCamera = m.device->GetCamera(DeviceDelegate::CameraEnum::Right);
    ControllerDelegatePtr delegate = m.controllers;
    m.device->SetClipPlanes(m.nearClip, m.farClip);
    m.device->SetControllerDelegate(delegate);
    m.gestures = m.device->GetGestureDelegate();
  } else if (previousDevice) {
    m.leftCamera = m.rightCamera = nullptr;
    for (Controller& controller: m.controllers->list) {
      if (controller.transform) {
        controller.transform->RemoveFromParents();
      }
      controller.Reset();

    }
    previousDevice->ReleaseControllerDelegate();
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

  if (!m.handleScrollEventMethod) {
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

  m.handleTrayEventMethod = m.env->GetMethodID(clazz, kHandleTrayEventName, kHandleTrayEventSignature);

  if (!m.handleTrayEventMethod) {
     VRB_LOG("Failed to find Java method: %s %s", kHandleTrayEventName, kHandleTrayEventSignature);
  }

  jmethodID getDisplayDensityMethod =  m.env->GetMethodID(clazz, kGetDisplayDensityName, kGetDisplayDensitySignature);
  if (getDisplayDensityMethod) {
    m.displayDensity = m.env->CallFloatMethod(m.activity, getDisplayDensityMethod);
  }

  m.InitializeWindows();

  if (!m.controllers->modelsLoaded) {
    const int32_t modelCount = m.device->GetControllerModelCount();
    for (int32_t index = 0; index < modelCount; index++) {
      const std::string fileName = m.device->GetControllerModelName(index);
      if (!fileName.empty()) {
        m.controllers->SetUpModelsGroup(index);
        m.factory->SetModelRoot(m.controllers->models[index]);
        m.parser->LoadModel(fileName);
      }
    }
    m.root->AddNode(m.controllers->root);
    CreateControllerPointer();
    CreateFloor();
    CreateTray();
    m.controllers->modelsLoaded = true;
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
  m.handleTrayEventMethod = nullptr;
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
                                    (int32_t)(placement.width * m.displayDensity),
                                    (int32_t)(placement.height * m.displayDensity),
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
BrowserWorld::CreateTray() {
  m.tray = Tray::Create(m.contextWeak);
  m.tray->Load(m.factory, m.parser);
  m.root->AddNode(m.tray->GetRoot());

  vrb::Matrix transform = vrb::Matrix::Rotation(vrb::Vector(1.0f, 0.0f, 0.0f), 30.0f * M_PI/180.0f);
  transform.TranslateInPlace(Vector(0.0f, 0.45f, -1.2f));
  m.tray->SetTransform(transform);
}

void
BrowserWorld::CreateControllerPointer() {
  if (m.controllers->pointerModel) {
    return;
  }
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

  m.controllers->pointerModel = std::move(geometry);
  for (Controller& controller: m.controllers->list) {
    if (controller.transform) {
      controller.transform->AddNode(m.controllers->pointerModel);
    }
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
