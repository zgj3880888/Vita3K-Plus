package org.vita3k.emulator.overlay;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.util.Log;
import android.view.InputDevice;

import androidx.annotation.RequiresApi;

import java.util.HashMap;
import java.util.Map;

/**
 * Reads the motion sensors a game pad carries on its own board and hands the samples to the
 * emulator.
 *
 * <p>SDL never reports these on Android. It reaches a pad through the platform's joystick
 * interface, and that interface says nothing about an accelerometer or a gyroscope, which is why
 * `SDL_GamepadHasSensor` comes back false for a Bluetooth pad even when it is full of them. The
 * platform does expose the sensors, but only per input device, through
 * {@link InputDevice#getSensorManager()} from API 31 on - the manager it returns belongs to the pad
 * rather than to the tablet. That is the one route that brings a pad's gyro into the emulator, and
 * with it the tilt that walking a wire or aiming by leaning asks for.
 *
 * <p>Pads are picked up as they are added and released as they are removed; a pad that is switched
 * between modes changes its sensors, so it is re-read on change as well. Nothing here runs below
 * Android 12, where the per-device manager does not exist.
 */
public final class GamepadMotion implements SensorEventListener {
  private static final String TAG = "GamepadMotion";

  // the two values the native side compares against SDL_SENSOR_ACCEL and SDL_SENSOR_GYRO
  private static final int SENSOR_ACCEL = 1;
  private static final int SENSOR_GYRO = 2;

  private final Context mContext;
  private final Map<Integer, Registration> mRegistrations = new HashMap<>();

  private InputManager mInputManager;
  private boolean mRunning;

  private static final class Registration {
    final SensorManager manager;
    final Sensor gyro;
    final Sensor accel;

    Registration(SensorManager manager, Sensor gyro, Sensor accel) {
      this.manager = manager;
      this.gyro = gyro;
      this.accel = accel;
    }
  }

  private final InputManager.InputDeviceListener mDeviceListener = new InputManager.InputDeviceListener() {
    @Override
    public void onInputDeviceAdded(int deviceId) {
      registerDevice(deviceId);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
      unregisterDevice(deviceId);
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
      // a pad can gain or lose its sensors when it is switched between modes
      unregisterDevice(deviceId);
      registerDevice(deviceId);
    }
  };

  public GamepadMotion(Context context) {
    mContext = context.getApplicationContext();
  }

  public void start() {
    if (mRunning)
      return;

    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
      Log.i(TAG, "reading a pad's own sensors needs Android 12 or newer; leaving them alone");
      return;
    }

    mInputManager = (InputManager) mContext.getSystemService(Context.INPUT_SERVICE);
    if (mInputManager == null)
      return;

    mRunning = true;
    mInputManager.registerInputDeviceListener(mDeviceListener, null);

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      for (int id : InputDevice.getDeviceIds())
        registerDevice(id);
    }
  }

  public void stop() {
    if (!mRunning)
      return;

    mRunning = false;
    if (mInputManager != null) {
      mInputManager.unregisterInputDeviceListener(mDeviceListener);
      mInputManager = null;
    }
    for (Integer id : mRegistrations.keySet().toArray(new Integer[0]))
      unregisterDevice(id);
  }

  @RequiresApi(Build.VERSION_CODES.S)
  private void registerDevice(int deviceId) {
    if (!mRunning || mRegistrations.containsKey(deviceId))
      return;

    InputDevice device = InputDevice.getDevice(deviceId);
    if (device == null || device.isVirtual())
      return;

    // pads only: the tablet's own sensors are already read natively, and taking them here as well
    // would put the same pose in twice
    final int sources = device.getSources();
    if ((sources & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) == 0)
      return;

    final SensorManager manager = device.getSensorManager();
    if (manager == null) {
      Log.w(TAG, "'" + device.getName() + "' is a pad but has no sensor manager; nothing to read");
      return;
    }

    // some pads only publish the uncalibrated variants, which carry the same three values first
    Sensor gyro = manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
    if (gyro == null)
      gyro = manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE_UNCALIBRATED);
    Sensor accel = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
    if (accel == null)
      accel = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER_UNCALIBRATED);
    if (gyro == null && accel == null) {
      Log.i(TAG, "'" + device.getName() + "' reports no gyroscope and no accelerometer of its own");
      return;
    }

    mRegistrations.put(deviceId, new Registration(manager, gyro, accel));
    if (gyro != null)
      manager.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME);
    if (accel != null)
      manager.registerListener(this, accel, SensorManager.SENSOR_DELAY_GAME);

    Log.i(TAG, "reading the sensors of '" + device.getName() + "'"
        + (gyro != null ? ", gyroscope" : "") + (accel != null ? ", accelerometer" : ""));
  }

  private void unregisterDevice(int deviceId) {
    final Registration registration = mRegistrations.remove(deviceId);
    if (registration == null)
      return;

    // this drops every sensor of this pad that this listener holds
    registration.manager.unregisterListener(this);
  }

  @Override
  public void onSensorChanged(SensorEvent event) {
    final int type = event.sensor.getType();
    if (type == Sensor.TYPE_GYROSCOPE || type == Sensor.TYPE_GYROSCOPE_UNCALIBRATED)
      pushSample(SENSOR_GYRO, event.values[0], event.values[1], event.values[2], event.timestamp);
    else if (type == Sensor.TYPE_ACCELEROMETER || type == Sensor.TYPE_ACCELEROMETER_UNCALIBRATED)
      pushSample(SENSOR_ACCEL, event.values[0], event.values[1], event.values[2], event.timestamp);
  }

  @Override
  public void onAccuracyChanged(Sensor sensor, int accuracy) {
  }

  private native void pushSample(int type, float x, float y, float z, long timestampNs);
}
