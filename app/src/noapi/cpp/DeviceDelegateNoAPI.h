/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DEVICE_DELEGATE_NO_API_DOT_H
#define DEVICE_DELEGATE_NO_API_DOT_H

#include "vrb/Forward.h"
#include "vrb/MacroUtils.h"
#include "DeviceDelegate.h"
#include <memory>

namespace crow {

class DeviceDelegateNoAPI;
typedef std::shared_ptr<DeviceDelegateNoAPI> DeviceDelegateNoAPIPtr;

class DeviceDelegateNoAPI : public DeviceDelegate {
public:
  static DeviceDelegateNoAPIPtr Create(vrb::ContextWeak aContext);
  // DeviceDelegate interface
  GestureDelegateConstPtr GetGestureDelegate() override;
  vrb::CameraPtr GetCamera(const CameraEnum aWhich) override;
  const vrb::Matrix& GetHeadTransform() const override;
  void SetClearColor(const vrb::Color& aColor) override;
  void SetClipPlanes(const float aNear, const float aFar) override;
  void SetControllerDelegate(ControllerDelegatePtr& aController) override;
  void ReleaseControllerDelegate() override;
  int32_t GetControllerModelCount() const override;
  const std::string GetControllerModelName(const int32_t aModelIndex) const override;
  void ProcessEvents() override;
  void StartFrame() override;
  void BindEye(const CameraEnum aWhich) override;
  void EndFrame() override;
  // DeviceDelegateNoAPI interface
  void SetViewport(const int aWidth, const int aHeight);
  void Pause();
  void Resume();
  void MoveAxis(const float aX, const float aY, const float aZ);
  void RotateHeading(const float aHeading);
  void TouchEvent(const bool aDown, const float aX, const float aY);
protected:
  struct State;
  DeviceDelegateNoAPI(State& aState);
  virtual ~DeviceDelegateNoAPI();
private:
  State& m;
  VRB_NO_DEFAULTS(DeviceDelegateNoAPI)
};

} // namespace crow
#endif // DEVICE_DELEGATE_NO_API_DOT_H
