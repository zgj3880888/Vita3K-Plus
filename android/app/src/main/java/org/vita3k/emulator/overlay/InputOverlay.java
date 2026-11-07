/*
 * Copyright 2013 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

package org.vita3k.emulator.overlay;

import org.vita3k.emulator.R;

import android.content.Context;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.View.OnTouchListener;

import java.util.HashSet;
import java.util.Set;

/**
 * Draws the interactive input overlay on top of the
 * {@link SurfaceView} that is rendering emulation.
 */
public final class InputOverlay extends SurfaceView implements OnTouchListener
{
  // mirror what is in controller_dialog.cpp
  public final static int OVERLAY_MASK_BASIC = 1;
  public final static int OVERLAY_MASK_L2R2 = 2;
  public final static int OVERLAY_MASK_L3R3 = 4;
  public final static int OVERLAY_MASK_TOUCH_SCREEN_SWITCH = 8;
  public final static int OVERLAY_MASK_HIDE_TOGGLE = 16;
  public final static int OVERLAY_MASK_TOUCH_MAP = 32;
  private final static int OVERLAY_MASK_UTILITY =
          OVERLAY_MASK_TOUCH_SCREEN_SWITCH | OVERLAY_MASK_HIDE_TOGGLE;

  // wait 10 seconds without inputs before hiding
  private final static int OVERLAY_TIME_BEFORE_HIDE = 10;

  private final Set<InputOverlayDrawableButton> overlayButtons = new HashSet<>();
  private final Set<InputOverlayDrawableDpad> overlayDpads = new HashSet<>();
  private final Set<InputOverlayDrawableJoystick> overlayJoysticks = new HashSet<>();
  private final Runnable mHideOverlayTicker = new Runnable() {
    @Override
    public void run() {
      tick();
      postDelayed(this, 1000);
    }
  };

  private int mOverlayMask = 0;
  private boolean mIsInEditMode = false;
  private boolean mAutoHideEnabled = true;
  private boolean mAllowVirtualController = true;
  private boolean mControllerAttached = false;
  private InputOverlayDrawableButton mButtonBeingConfigured;
  private InputOverlayDrawableDpad mDpadBeingConfigured;
  private InputOverlayDrawableJoystick mJoystickBeingConfigured;
  private float mScale = 1.0f;
  private int mOpacity = 100;
  private boolean mTouchSwitchTilt = false;
  private boolean mPadTouchMapping = true;
  private OverlayLayout mLayout;
  // where each touch mapping point was first pressed, so a drag can be replayed as a slide
  private final float[] mTouchZoneDownX = new float[ButtonType.TOUCH_MAP_COUNT];
  private final float[] mTouchZoneDownY = new float[ButtonType.TOUCH_MAP_COUNT];

  // Points 2, 3 and 4 sweep instead of tapping: the gesture some prompts ask for is a finger
  // travelling across the panel, which a finger resting on one spot never satisfies. Point 1 keeps
  // tapping, so a plain press is still available.
  private static final float TOUCH_MAP_SWIPE_SPAN = 0.28f;
  private static final float[] TOUCH_MAP_SWIPE_DX = { 0f, -1f, 1f, 0f };
  private static final float[] TOUCH_MAP_SWIPE_DY = { 0f, 0f, 0f, 1f };

  // last Time the screen was touched
  private long mlastTouchTime;
  // is the overlay hidden because we didn't used it for long enough ?
  private boolean mShowingOverlay = true;
  // hide overlay manually while keeping the touch utility buttons available
  private boolean mHideOverlayButtons = false;

  /**
   * Resizes a {@link Bitmap} by a given scale factor
   *
   * @param context The current {@link Context}
   * @param bitmap  The {@link Bitmap} to scale.
   * @param scale   The scale factor for the bitmap.
   * @return The scaled {@link Bitmap}
   */
  public static Bitmap resizeBitmap(Context context, Bitmap bitmap, float scale)
  {
    DisplayMetrics dm = context.getResources().getDisplayMetrics();
    int minDimension = Math.min(dm.widthPixels, dm.heightPixels);
    int maxBitmapDimension = Math.max(bitmap.getWidth(), bitmap.getHeight());
    float bitmapScale = scale * minDimension / maxBitmapDimension;

    return Bitmap.createScaledBitmap(bitmap,
            (int) (bitmap.getWidth() * bitmapScale),
            (int) (bitmap.getHeight() * bitmapScale),
            true);
  }

  /**
   * Constructor
   *
   * @param context The current {@link Context}.
   * @param attrs   {@link AttributeSet} for parsing XML attributes.
   */
  public InputOverlay(Context context/*, AttributeSet attrs*/)
  {
    super(context/*, attrs*/);

    // Force draw
    setWillNotDraw(false);

    // Request focus for the overlay so it has priority on presses.
    requestFocus();

    applyResolvedState(OverlayStore.loadGlobalState(getContext()), false);

    refreshControls();
    resetHideTimer();
  }

  @Override
  protected void onSizeChanged(int w, int h, int oldw, int oldh) {
    super.onSizeChanged(w, h, oldw, oldh);
    if ((w != oldw || h != oldh) && w > 0 && h > 0) {
      refreshControls();
    }
  }

  private void startHideTimer() {
    removeCallbacks(mHideOverlayTicker);
    postDelayed(mHideOverlayTicker, 1000);
  }

  private void stopHideTimer() {
    removeCallbacks(mHideOverlayTicker);
  }

  @Override
  protected void onAttachedToWindow() {
    super.onAttachedToWindow();
    startHideTimer();
    updateVirtualControllerState();
  }

  @Override
  protected void onDetachedFromWindow() {
    stopHideTimer();
    releaseAllInputs();
    detachVirtualController();
    super.onDetachedFromWindow();
  }

  private void resetHideTimer(){
    if(!mShowingOverlay)
      invalidate();

    mShowingOverlay = true;
    mlastTouchTime = System.currentTimeMillis();
  }

  public void tick(){
    if(mOverlayMask == 0 || !mShowingOverlay || isInEditMode() || !mAutoHideEnabled)
      return;

    long current_time = System.currentTimeMillis();
    if (current_time - mlastTouchTime >= OVERLAY_TIME_BEFORE_HIDE * 1000) {
      mShowingOverlay = false;
      invalidate();
    }
  }

  public void setState(int overlay_mask){
    if(mOverlayMask != overlay_mask){
      mOverlayMask = overlay_mask;
      releaseInactiveInputs();
      invalidate();
    }

    resetHideTimer();
    updateVirtualControllerState();
  }

  public int getOverlayMask() {
    return mOverlayMask;
  }

  public void releaseAllInputs() {
    for (InputOverlayDrawableButton button : overlayButtons) {
      if (isTouchMapZone(button.getLegacyId())) {
        releaseTouchZone(button);
      } else if ((button.getRole() & OVERLAY_MASK_UTILITY) == 0) {
        setButton(button.getControl(), false);
      }
      button.setPressedState(false);
      button.setTrackId(-1);
    }

    for (InputOverlayDrawableDpad dpad : overlayDpads) {
      for (int i = 0; i < 4; i++) {
        setButton(dpad.getControl(i), false);
      }
      dpad.setState(InputOverlayDrawableDpad.STATE_DEFAULT);
      dpad.setTrackId(-1);
    }

    for (InputOverlayDrawableJoystick joystick : overlayJoysticks) {
      setAxis(joystick.getXControl(), (short)0);
      setAxis(joystick.getYControl(), (short)0);
      joystick.release();
    }

    invalidate();
  }

  private void releaseButton(InputOverlayDrawableButton button)
  {
    int legacyId = button.getLegacyId();
    button.setPressedState(false);
    if (isTouchMapZone(legacyId))
      releaseTouchZone(button);
    else if (legacyId != ButtonType.BUTTON_TOUCH_SWITCH && legacyId != ButtonType.BUTTON_TOUCH_HIDE)
      setButton(button.getControl(), false);
    button.setTrackId(-1);
  }

  private void releaseDpad(InputOverlayDrawableDpad dpad)
  {
    for (int i = 0; i < 4; i++)
      setButton(dpad.getControl(i), false);
    dpad.setState(InputOverlayDrawableDpad.STATE_DEFAULT);
    dpad.setTrackId(-1);
  }

  private void releaseJoystick(InputOverlayDrawableJoystick joystick)
  {
    setAxis(joystick.getXControl(), (short)0);
    setAxis(joystick.getYControl(), (short)0);
    joystick.release();
  }

  private void releaseTrackedInputs(int pointerId)
  {
    boolean released = false;
    for (InputOverlayDrawableButton button : overlayButtons)
    {
      if (button.getTrackId() != -1 && (pointerId == -1 || button.getTrackId() == pointerId))
      {
        releaseButton(button);
        released = true;
      }
    }
    for (InputOverlayDrawableDpad dpad : overlayDpads)
    {
      if (dpad.getTrackId() != -1 && (pointerId == -1 || dpad.getTrackId() == pointerId))
      {
        releaseDpad(dpad);
        released = true;
      }
    }
    for (InputOverlayDrawableJoystick joystick : overlayJoysticks)
    {
      if (joystick.getTrackId() != -1 && (pointerId == -1 || joystick.getTrackId() == pointerId))
      {
        releaseJoystick(joystick);
        released = true;
      }
    }
    if (released)
      invalidate();
  }

  private void releaseInactiveInputs()
  {
    final boolean processing = mOverlayMask != 0 && (mAllowVirtualController || isInEditMode());
    boolean released = false;
    for (InputOverlayDrawableButton button : overlayButtons)
    {
      final int role = button.getRole();
      final boolean active = processing && (role & mOverlayMask) != 0
              && (!mHideOverlayButtons || (role & OVERLAY_MASK_UTILITY) != 0);
      if (!active && button.getTrackId() != -1)
      {
        releaseButton(button);
        released = true;
      }
    }
    if (!processing || mHideOverlayButtons)
    {
      for (InputOverlayDrawableDpad dpad : overlayDpads)
      {
        if (dpad.getTrackId() != -1)
        {
          releaseDpad(dpad);
          released = true;
        }
      }
      for (InputOverlayDrawableJoystick joystick : overlayJoysticks)
      {
        if (joystick.getTrackId() != -1)
        {
          releaseJoystick(joystick);
          released = true;
        }
      }
    }
    if (released)
      invalidate();
  }

  public void draw(Canvas canvas)
  {
    super.draw(canvas);

    if (mOverlayMask == 0 || !mShowingOverlay)
      return;

    for (InputOverlayDrawableButton button : overlayButtons)
    {
      int role = button.getRole();

      if (mHideOverlayButtons) {
        if ((role & OVERLAY_MASK_UTILITY) == 0 || (role & mOverlayMask) == 0)
          continue;
      } else if ((role & mOverlayMask) == 0) {
        continue;
      }

      button.draw(canvas);
    }

    if (!mHideOverlayButtons)
    {
      for (InputOverlayDrawableDpad dpad : overlayDpads)
      {
        dpad.draw(canvas);
      }

      for (InputOverlayDrawableJoystick joystick : overlayJoysticks)
      {
        joystick.draw(canvas);
      }
    }
  }

  @Override
  public boolean onTouch(View v, MotionEvent event)
  {
    final int actionMasked = event.getActionMasked();
    if (actionMasked == MotionEvent.ACTION_CANCEL)
    {
      releaseAllInputs();
      return false;
    }
    if (actionMasked == MotionEvent.ACTION_DOWN)
      releaseTrackedInputs(-1);
    else if (actionMasked == MotionEvent.ACTION_POINTER_DOWN)
      releaseTrackedInputs(event.getPointerId(event.getActionIndex()));

    if (mOverlayMask == 0)
      return false;

    if(!mAllowVirtualController && !isInEditMode())
      return false;

    resetHideTimer();

    if (isInEditMode())
      return onTouchWhileEditing(event);

    int action = event.getActionMasked();
    boolean firstPointer = action != MotionEvent.ACTION_POINTER_DOWN &&
            action != MotionEvent.ACTION_POINTER_UP;
    int pointerIndex = firstPointer ? 0 : event.getActionIndex();
    // track if the overlay is concerned this this action
    boolean concerned = false;
    boolean hideToggled = false;

    // a touch mapping point that is dragged slides its finger along with the pointer
    if (action == MotionEvent.ACTION_MOVE && moveTouchZones(event))
      concerned = true;



    for (InputOverlayDrawableButton button : overlayButtons)
    {
      int buttonRole = button.getRole();
      int legacyId = button.getLegacyId();

      if (mHideOverlayButtons &&
              (((buttonRole & OVERLAY_MASK_UTILITY) == 0) || (buttonRole & mOverlayMask) == 0))
          continue;
      else if ((buttonRole & mOverlayMask) == 0)
          continue;

      // Determine the button state to apply based on the MotionEvent action flag.
      switch (action)
      {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
          // If a pointer enters the bounds of a button, press that button.
          if (button.getBounds()
                  .contains((int) event.getX(pointerIndex), (int) event.getY(pointerIndex)))
          {
            button.setPressedState(true);
            button.setTrackId(event.getPointerId(pointerIndex));
            concerned = true;

            if (legacyId == ButtonType.BUTTON_TOUCH_SWITCH)
              setTouchState(button.getTouchSwitchState());
            else if (legacyId == ButtonType.BUTTON_TOUCH_HIDE)
            {
              mHideOverlayButtons = !mHideOverlayButtons;
              hideToggled = true;
            }
            else if (isTouchMapZone(legacyId))
              pressTouchZone(button, event.getX(pointerIndex), event.getY(pointerIndex));
            else
              setButton(button.getControl(), true);
          }
          break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
          // If a pointer ends, release the button it was pressing.
          if (button.getTrackId() == event.getPointerId(pointerIndex))
          {
            button.setPressedState(false);

            if (isTouchMapZone(legacyId))
              releaseTouchZone(button);
            else if (legacyId != ButtonType.BUTTON_TOUCH_SWITCH &&
                    legacyId != ButtonType.BUTTON_TOUCH_HIDE)
              setButton(button.getControl(), false);

            button.setTrackId(-1);
            concerned = true;
          }
          break;
      }
    }

    if (!mHideOverlayButtons)
    {
      for (InputOverlayDrawableDpad dpad : overlayDpads)
      {
        // Determine the button state to apply based on the MotionEvent action flag.
        switch (event.getAction() & MotionEvent.ACTION_MASK)
        {
          case MotionEvent.ACTION_DOWN:
          case MotionEvent.ACTION_POINTER_DOWN:
            // If a pointer enters the bounds of a button, press that button.
            if (dpad.getBounds()
                    .contains((int) event.getX(pointerIndex), (int) event.getY(pointerIndex)))
            {
              dpad.setTrackId(event.getPointerId(pointerIndex));
              concerned = true;
            }
          case MotionEvent.ACTION_MOVE:
            if (dpad.getTrackId() == event.getPointerId(pointerIndex))
            {
              concerned = true;
              // Up, Down, Left, Right
              boolean[] dpadPressed = {false, false, false, false};

              if (dpad.getBounds().top + (dpad.getHeight() / 3) > (int) event.getY(pointerIndex))
                dpadPressed[0] = true;
              if (dpad.getBounds().bottom - (dpad.getHeight() / 3) < (int) event.getY(pointerIndex))
                dpadPressed[1] = true;
              if (dpad.getBounds().left + (dpad.getWidth() / 3) > (int) event.getX(pointerIndex))
                dpadPressed[2] = true;
              if (dpad.getBounds().right - (dpad.getWidth() / 3) < (int) event.getX(pointerIndex))
                dpadPressed[3] = true;

              // Press buttons
              for (int i = 0; i < dpadPressed.length; i++)
              {
                if (dpadPressed[i])
                  setButton(dpad.getControl(i), true);
              }
              setDpadState(dpad, dpadPressed[0], dpadPressed[1], dpadPressed[2], dpadPressed[3]);
            }
            break;
          case MotionEvent.ACTION_UP:
          case MotionEvent.ACTION_POINTER_UP:
            // If a pointer ends, release the buttons.
            if (dpad.getTrackId() == event.getPointerId(pointerIndex))
            {
              concerned = true;
              for (int i = 0; i < 4; i++)
              {
                dpad.setState(InputOverlayDrawableDpad.STATE_DEFAULT);
                setButton(dpad.getControl(i), false);
              }
              dpad.setTrackId(-1);
            }
            break;
        }
      }

      for (InputOverlayDrawableJoystick joystick : overlayJoysticks)
      {
        if (joystick.TrackEvent(event))
        {
          concerned = true;

          int joyX = Math.round(joystick.getX() * (1 << 15));
          joyX = Math.max(Short.MIN_VALUE, Math.min(Short.MAX_VALUE, joyX));
          int joyY = Math.round(joystick.getY() * (1 << 15));
          joyY = Math.max(Short.MIN_VALUE, Math.min(Short.MAX_VALUE, joyY));
          setAxis(joystick.getXControl(), (short)joyX);
          setAxis(joystick.getYControl(), (short)joyY);
        }
      }
    }

    if (hideToggled)
      releaseInactiveInputs();

    if (concerned)
      invalidate();

    return concerned;
  }

  public boolean onTouchWhileEditing(MotionEvent event)
  {
    int pointerIndex = event.getActionIndex();
    int fingerPositionX = (int) event.getX(pointerIndex);
    int fingerPositionY = (int) event.getY(pointerIndex);

    // Maybe combine Button and Joystick as subclasses of the same parent?
    // Or maybe create an interface like IMoveableHUDControl?
    boolean intersect = false;

    for (InputOverlayDrawableButton button : overlayButtons)
    {
      // Determine the button state to apply based on the MotionEvent action flag.
      switch (event.getAction() & MotionEvent.ACTION_MASK)
      {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
          // If no button is being moved now, remember the currently touched button to move.
          if (mButtonBeingConfigured == null &&
                  button.getBounds().contains(fingerPositionX, fingerPositionY))
          {
            mButtonBeingConfigured = button;
            mButtonBeingConfigured.onConfigureTouch(event);
            intersect = true;
          }
          break;
        case MotionEvent.ACTION_MOVE:
          if (mButtonBeingConfigured != null)
          {
            mButtonBeingConfigured.onConfigureTouch(event);
            invalidate();
            return true;
          }
          break;

        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
          if (mButtonBeingConfigured == button)
          {
            // Persist button position by saving new place.
            saveControlPosition(mButtonBeingConfigured.getLegacyId(),
                    mButtonBeingConfigured.getBounds().left,
                    mButtonBeingConfigured.getBounds().top);
            mButtonBeingConfigured = null;
            intersect = true;
          }
          break;
      }
    }

    for (InputOverlayDrawableDpad dpad : overlayDpads)
    {
      // Determine the button state to apply based on the MotionEvent action flag.
      switch (event.getAction() & MotionEvent.ACTION_MASK)
      {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
          // If no button is being moved now, remember the currently touched button to move.
          if (mButtonBeingConfigured == null &&
                  dpad.getBounds().contains(fingerPositionX, fingerPositionY))
          {
            mDpadBeingConfigured = dpad;
            mDpadBeingConfigured.onConfigureTouch(event);
            intersect = true;
          }
          break;
        case MotionEvent.ACTION_MOVE:
          if (mDpadBeingConfigured != null)
          {
            mDpadBeingConfigured.onConfigureTouch(event);
            invalidate();
            return true;
          }
          break;

        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
          if (mDpadBeingConfigured == dpad)
          {
            // Persist button position by saving new place.
            saveControlPosition(mDpadBeingConfigured.getLegacyId(),
                    mDpadBeingConfigured.getBounds().left,
                    mDpadBeingConfigured.getBounds().top);
            mDpadBeingConfigured = null;
            intersect = true;
          }
          break;
      }
    }

    for (InputOverlayDrawableJoystick joystick : overlayJoysticks)
    {
      switch (event.getAction())
      {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
          if (mJoystickBeingConfigured == null &&
                  joystick.getBounds().contains(fingerPositionX, fingerPositionY))
          {
            mJoystickBeingConfigured = joystick;
            mJoystickBeingConfigured.onConfigureTouch(event);
            intersect = true;
          }
          break;
        case MotionEvent.ACTION_MOVE:
          if (mJoystickBeingConfigured != null)
          {
            mJoystickBeingConfigured.onConfigureTouch(event);
            invalidate();
            intersect = true;
          }
          break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
          if (mJoystickBeingConfigured != null)
          {
            saveControlPosition(mJoystickBeingConfigured.getLegacyId(),
                    mJoystickBeingConfigured.getBounds().left,
                    mJoystickBeingConfigured.getBounds().top);
            mJoystickBeingConfigured = null;
            intersect = true;
          }
          break;
      }
    }

    return intersect;
  }

  private void setDpadState(InputOverlayDrawableDpad dpad, boolean up, boolean down, boolean left,
          boolean right)
  {
    if (up)
    {
      if (left)
        dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_UP_LEFT);
      else if (right)
        dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_UP_RIGHT);
      else
        dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_UP);
    }
    else if (down)
    {
      if (left)
        dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_DOWN_LEFT);
      else if (right)
        dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_DOWN_RIGHT);
      else
        dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_DOWN);
    }
    else if (left)
    {
      dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_LEFT);
    }
    else if (right)
    {
      dpad.setState(InputOverlayDrawableDpad.STATE_PRESSED_RIGHT);
    }
  }


  private void addVitaOverlayControls(LayoutBounds layoutBounds, OverlayLayout layout)
  {
      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_cross,
              R.drawable.button_cross_pressed, ButtonType.BUTTON_CROSS, ControlId.a,
              OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));

      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_circle,
              R.drawable.button_circle_pressed, ButtonType.BUTTON_CIRCLE, ControlId.b,
              OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));

      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_square,
              R.drawable.button_square_pressed, ButtonType.BUTTON_SQUARE, ControlId.x,
              OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));

      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_triangle,
              R.drawable.button_triangle_pressed, ButtonType.BUTTON_TRIANGLE, ControlId.y,
              OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));
      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_start,
              R.drawable.button_start_pressed, ButtonType.BUTTON_START,
              ControlId.start, OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));

      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_ps,
            R.drawable.button_ps_pressed, ButtonType.BUTTON_PS,
            ControlId.guide, OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));

      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_select,
              R.drawable.button_select_pressed, ButtonType.BUTTON_SELECT,
              ControlId.select, OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));

      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_l,
              R.drawable.button_l_pressed, ButtonType.TRIGGER_L,
              ControlId.l1, OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));
      overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_r,
              R.drawable.button_r_pressed, ButtonType.TRIGGER_R,
              ControlId.r1, OVERLAY_MASK_BASIC, layout, layoutBounds, mScale, mOpacity));

    overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_l2,
            R.drawable.button_l2_pressed, ButtonType.TRIGGER_L2,
            ControlId.l2, OVERLAY_MASK_L2R2, layout, layoutBounds, mScale, mOpacity));

    overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_r2,
            R.drawable.button_r2_pressed, ButtonType.TRIGGER_R2,
            ControlId.r2, OVERLAY_MASK_L2R2, layout, layoutBounds, mScale, mOpacity));

    // L3 and R3
    overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_l3,
            R.drawable.button_l3_pressed, ButtonType.TRIGGER_L3,
            ControlId.l3, OVERLAY_MASK_L3R3, layout, layoutBounds, mScale, mOpacity));

    overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_r3,
            R.drawable.button_r3_pressed, ButtonType.TRIGGER_R3,
            ControlId.r3, OVERLAY_MASK_L3R3, layout, layoutBounds, mScale, mOpacity));

    final InputOverlayDrawableButton touchSwitch = initializeOverlayButton(getContext(), R.drawable.button_touch_f,
            R.drawable.button_touch_b, ButtonType.BUTTON_TOUCH_SWITCH,
            ControlId.touch, OVERLAY_MASK_TOUCH_SCREEN_SWITCH, layout, layoutBounds, mScale, mOpacity);
    if (mTouchSwitchTilt)
      touchSwitch.setTiltStateBitmap(getResources(), BitmapFactory.decodeResource(getResources(), R.drawable.button_touch_t));
    overlayButtons.add(touchSwitch);

    // show hide button
    overlayButtons.add(initializeOverlayButton(getContext(), R.drawable.button_hide,
            R.drawable.button_hide_pressed, ButtonType.BUTTON_TOUCH_HIDE,
            ControlId.touch, OVERLAY_MASK_HIDE_TOGGLE, layout, layoutBounds, mScale, mOpacity));

    // touch mapping points: pressing one reports a finger held on the touch panel
    for (int i = 0; i < ButtonType.TOUCH_MAP_COUNT; i++)
      overlayButtons.add(initializeTouchMapButton(getContext(), ButtonType.TOUCH_MAP_1 + i,
              layout, layoutBounds, mScale, mOpacity, i));

      overlayDpads.add(initializeOverlayDpad(getContext(), R.drawable.dpad_idle,
              R.drawable.dpad_up,
              R.drawable.dpad_up_left,
              ButtonType.DPAD_UP, ControlId.dup, ControlId.ddown,
              ControlId.dleft, ControlId.dright, layout, layoutBounds, mScale, mOpacity));

      overlayJoysticks.add(initializeOverlayJoystick(getContext(), R.drawable.joystick_range,
              R.drawable.joystick, R.drawable.joystick_pressed,
              ButtonType.STICK_LEFT, ControlId.axis_left_x,
              ControlId.axis_left_y, layout, layoutBounds, mScale, mOpacity));
      overlayJoysticks.add(initializeOverlayJoystick(getContext(), R.drawable.joystick_range,
              R.drawable.joystick, R.drawable.joystick_pressed,
              ButtonType.STICK_RIGHT, ControlId.axis_right_x,
              ControlId.axis_right_y, layout, layoutBounds, mScale, mOpacity));
  }

  public void refreshControls()
  {
    int touchSwitchState = 0;
    for (InputOverlayDrawableButton button : overlayButtons)
      if (button.getLegacyId() == ButtonType.BUTTON_TOUCH_SWITCH)
        touchSwitchState = button.getTouchSwitchState();

    // Remove all the overlay buttons from the HashSet.
    overlayButtons.clear();
    overlayDpads.clear();
    overlayJoysticks.clear();
    // the controls are rebuilt from scratch, so any finger a touch mapping point was holding is gone
    clearMappedTouch();

    LayoutBounds layoutBounds = resolveLayoutBounds();
    addVitaOverlayControls(layoutBounds, mLayout);

    for (InputOverlayDrawableButton button : overlayButtons)
    {
      if (button.getLegacyId() != ButtonType.BUTTON_TOUCH_SWITCH)
        continue;
      final int restored = touchSwitchState < button.getTouchSwitchStateCount() ? touchSwitchState : 0;
      button.setTouchSwitchState(restored);
      if (restored != touchSwitchState)
        setTouchState(restored);
    }

    // hand the new places to the native side so L3/R3 can hold them down straight away
    syncTouchMapPositions();
    applyTouchMapSweeps();
    // repeated on every rebuild on purpose: the native session may not have existed when the
    // preference was first applied, and this is where it becomes live
    setPadTouchMapping(mPadTouchMapping);

    invalidate();
  }

  public void resetButtonPlacement()
  {
    setLayout(OverlayStore.defaultLayout(getContext()), true);
  }

  public void setScale(float scale){
    if (scale != mScale){
      mScale = scale;
      refreshControls();
    }
  }

  public void setOpacity(int opacity){
    if (opacity != mOpacity){
      mOpacity = opacity;
      refreshControls();
    }
  }

  private void saveControlPosition(int buttonType, int x, int y)
  {
    LayoutBounds layoutBounds = resolveLayoutBounds();
    float normalizedX = layoutBounds.width > 0 ? (float) x / layoutBounds.width : 0f;
    float normalizedY = layoutBounds.height > 0 ? (float) y / layoutBounds.height : 0f;
    OverlayLayout updatedLayout = mLayout.withPosition(buttonType, normalizedX, normalizedY);
    setLayout(updatedLayout, false);
    // dragging a ring does not rebuild the controls, so push the new place by hand
    if (isTouchMapZone(buttonType))
      syncTouchMapPositions();
  }

  /**
   * Initializes an InputOverlayDrawableButton, given by resId, with all of the
   * parameters set for it to be properly shown on the InputOverlay.
   * <p>
   * Position and appearance are fully resolved before returning the drawable.
   *
   * @param context      The current {@link Context}.
   * @param defaultResId The resource ID of the {@link Drawable} to get the {@link Bitmap} of (Default State).
   * @param pressedResId The resource ID of the {@link Drawable} to get the {@link Bitmap} of (Pressed State).
   * @param legacyId     Legacy identifier for the button the InputOverlayDrawableButton represents.
   * @param control      Control identifier for the button the InputOverlayDrawableButton represents.
   * @return An {@link InputOverlayDrawableButton} with the correct drawing bounds set.
   */
  private static InputOverlayDrawableButton initializeOverlayButton(Context context,
          int defaultResId, int pressedResId, int legacyId, int control, int role,
          OverlayLayout layout, LayoutBounds layoutBounds, float globalScale, int globalOpacity)
  {
    // Resources handle for fetching the initial Drawable resource.
    final Resources res = context.getResources();

    // Decide scale based on button ID and user preference
    float scale = 0.15f;

    if(legacyId == ButtonType.TRIGGER_L
            || legacyId == ButtonType.TRIGGER_R
            || legacyId == ButtonType.TRIGGER_L2
            || legacyId == ButtonType.TRIGGER_R2
            || legacyId == ButtonType.BUTTON_START
            || legacyId == ButtonType.BUTTON_SELECT)
      scale = 0.25f;
    else if(legacyId == ButtonType.BUTTON_TOUCH_SWITCH
            || legacyId == ButtonType.BUTTON_PS
            || legacyId == ButtonType.BUTTON_TOUCH_HIDE)
      scale = 0.11f;

    scale *= globalScale;

    // Initialize the InputOverlayDrawableButton.
    final Bitmap defaultStateBitmap =
            resizeBitmap(context, BitmapFactory.decodeResource(res, defaultResId), scale);
    final Bitmap pressedStateBitmap =
            resizeBitmap(context, BitmapFactory.decodeResource(res, pressedResId), scale);
    final InputOverlayDrawableButton overlayDrawable =
            new InputOverlayDrawableButton(res, defaultStateBitmap, pressedStateBitmap, legacyId,
                    control, role);

    if (legacyId == ButtonType.BUTTON_TOUCH_SWITCH)
    {
      final Bitmap bothStateBitmap = resizeBitmap(context, BitmapFactory.decodeResource(res, R.drawable.button_touch_fb), scale);
      overlayDrawable.setThirdStateBitmap(res, bothStateBitmap);
    }

    OverlayPosition position = layout.positionFor(legacyId);
    if (position == null)
      position = new OverlayPosition(0f, 0f);
    int drawableX = Math.round(position.getNormalizedX() * layoutBounds.width);
    int drawableY = Math.round(position.getNormalizedY() * layoutBounds.height);

    int width = overlayDrawable.getWidth();
    int height = overlayDrawable.getHeight();

    // Now set the bounds for the InputOverlayDrawableButton.
    // This will dictate where on the screen (and the what the size) the InputOverlayDrawableButton will be.
    overlayDrawable.setBounds(drawableX, drawableY, drawableX + width, drawableY + height);

    // Need to set the image's position
    overlayDrawable.setPosition(drawableX, drawableY);
    overlayDrawable.setOpacity((int) (globalOpacity * 0.01 * 255));

    return overlayDrawable;
  }

  private static boolean isTouchMapZone(int legacyId)
  {
    return legacyId >= ButtonType.TOUCH_MAP_1
            && legacyId < ButtonType.TOUCH_MAP_1 + ButtonType.TOUCH_MAP_COUNT;
  }

  private static int touchMapSlot(int legacyId)
  {
    return isTouchMapZone(legacyId) ? legacyId - ButtonType.TOUCH_MAP_1 : -1;
  }

  private static boolean isTouchMapSwipe(int slot)
  {
    return slot >= 0 && slot < TOUCH_MAP_SWIPE_DX.length
            && (TOUCH_MAP_SWIPE_DX[slot] != 0f || TOUCH_MAP_SWIPE_DY[slot] != 0f);
  }

  /**
   * Builds the ring used for a touch mapping point. Drawn in code so no extra drawable is needed,
   * and numbered so several points can be told apart.
   */
  private static Bitmap createTouchMapBitmap(int index, boolean pressed)
  {
    final int size = 256;
    final float center = size / 2f;
    final int accent = pressed ? 0xFFFF7A18 : 0xFFFFFFFF;
    final Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
    final Canvas canvas = new Canvas(bitmap);

    final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    paint.setStyle(Paint.Style.FILL);
    paint.setColor(pressed ? 0xFFFF7A18 : 0x59101010);
    canvas.drawCircle(center, center, size * 0.40f, paint);

    paint.setStyle(Paint.Style.STROKE);
    paint.setStrokeWidth(size * 0.055f);
    paint.setColor(accent);
    canvas.drawCircle(center, center, size * 0.40f, paint);

    // ticks just outside the ring make the exact touched spot easy to line up
    paint.setStrokeWidth(size * 0.03f);
    paint.setColor(pressed ? 0xFFB33C00 : 0x8CFFFFFF);

    final float swipeDx = (index >= 0 && index < TOUCH_MAP_SWIPE_DX.length) ? TOUCH_MAP_SWIPE_DX[index] : 0f;
    final float swipeDy = (index >= 0 && index < TOUCH_MAP_SWIPE_DY.length) ? TOUCH_MAP_SWIPE_DY[index] : 0f;
    if (swipeDx != 0f || swipeDy != 0f) {
      // a sweep point wears an arrow instead of ticks: it slides, it does not sit
      paint.setStyle(Paint.Style.STROKE);
      paint.setStrokeWidth(size * 0.032f);
      paint.setColor(accent);
      canvas.drawLine(center + swipeDx * size * 0.430f, center + swipeDy * size * 0.430f,
              center + swipeDx * size * 0.480f, center + swipeDy * size * 0.480f, paint);

      final float tipX = center + swipeDx * size * 0.512f;
      final float tipY = center + swipeDy * size * 0.512f;
      final float baseX = center + swipeDx * size * 0.478f;
      final float baseY = center + swipeDy * size * 0.478f;
      final float perpX = -swipeDy * size * 0.032f;
      final float perpY = swipeDx * size * 0.032f;
      canvas.drawLine(tipX, tipY, baseX + perpX, baseY + perpY, paint);
      canvas.drawLine(tipX, tipY, baseX - perpX, baseY - perpY, paint);
    } else {
      for (int tick = 0; tick < 4; tick++) {
        final double angle = Math.PI / 2 * tick;
        final float dx = (float) Math.cos(angle);
        final float dy = (float) Math.sin(angle);
        canvas.drawLine(center + dx * size * 0.455f, center + dy * size * 0.455f,
                center + dx * size * 0.495f, center + dy * size * 0.495f, paint);
      }
    }

    paint.setStyle(Paint.Style.FILL);
    paint.setTextAlign(Paint.Align.CENTER);
    paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
    paint.setTextSize(size * 0.42f);
    paint.setColor(pressed ? 0xFF101010 : accent);
    final float baseline = center - (paint.descent() + paint.ascent()) / 2f;
    canvas.drawText(Integer.toString(index + 1), center, baseline, paint);

    return bitmap;
  }

  private static InputOverlayDrawableButton initializeTouchMapButton(Context context, int legacyId,
          OverlayLayout layout, LayoutBounds layoutBounds, float globalScale, int globalOpacity,
          int index)
  {
    final Resources res = context.getResources();
    final float scale = 0.13f * globalScale;

    final Bitmap defaultStateBitmap =
            resizeBitmap(context, createTouchMapBitmap(index, false), scale);
    final Bitmap pressedStateBitmap =
            resizeBitmap(context, createTouchMapBitmap(index, true), scale);

    // control -1: this button never presses anything on the virtual pad, it pokes the touch panel
    final InputOverlayDrawableButton overlayDrawable =
            new InputOverlayDrawableButton(res, defaultStateBitmap, pressedStateBitmap, legacyId,
                    -1, OVERLAY_MASK_TOUCH_MAP);

    OverlayPosition position = layout.positionFor(legacyId);
    if (position == null)
      position = new OverlayPosition(0.5f, 0.5f);
    final int drawableX = Math.round(position.getNormalizedX() * layoutBounds.width);
    final int drawableY = Math.round(position.getNormalizedY() * layoutBounds.height);

    overlayDrawable.setBounds(drawableX, drawableY,
            drawableX + overlayDrawable.getWidth(), drawableY + overlayDrawable.getHeight());
    overlayDrawable.setPosition(drawableX, drawableY);
    overlayDrawable.setOpacity((int) (globalOpacity * 0.01 * 255));

    return overlayDrawable;
  }

  /**
   * Reports a finger at the centre of the point, expressed in the same normalized space as the
   * surface touch events so the native side can map it through the display viewport.
   */
  private void pressTouchZone(InputOverlayDrawableButton button, float downX, float downY)
  {
    final int slot = touchMapSlot(button.getLegacyId());
    if (slot < 0)
      return;

    mTouchZoneDownX[slot] = downX;
    mTouchZoneDownY[slot] = downY;

    final int width = Math.max(1, getWidth());
    final int height = Math.max(1, getHeight());
    final Rect bounds = button.getBounds();
    setMappedTouch(slot, bounds.exactCenterX() / width, bounds.exactCenterY() / height, true);
  }

  private void updateTouchZone(InputOverlayDrawableButton button, float x, float y)
  {
    final int slot = touchMapSlot(button.getLegacyId());
    if (slot < 0)
      return;

    // a sweep already left the centre the moment it was pressed; following the finger would restart
    // it every frame and pin the gesture at the first step
    if (isTouchMapSwipe(slot))
      return;

    final int width = Math.max(1, getWidth());
    final int height = Math.max(1, getHeight());
    final Rect bounds = button.getBounds();
    setMappedTouch(slot,
            (bounds.exactCenterX() + (x - mTouchZoneDownX[slot])) / width,
            (bounds.exactCenterY() + (y - mTouchZoneDownY[slot])) / height,
            true);
  }

  private void releaseTouchZone(InputOverlayDrawableButton button)
  {
    final int slot = touchMapSlot(button.getLegacyId());
    if (slot < 0)
      return;

    setMappedTouch(slot, 0f, 0f, false);
  }

  private boolean moveTouchZones(MotionEvent event)
  {
    boolean moved = false;
    for (int i = 0; i < event.getPointerCount(); i++) {
      final int pointerId = event.getPointerId(i);
      for (InputOverlayDrawableButton button : overlayButtons) {
        if (!isTouchMapZone(button.getLegacyId()) || button.getTrackId() != pointerId)
          continue;
        updateTouchZone(button, event.getX(i), event.getY(i));
        moved = true;
      }
    }
    return moved;
  }

  /**
   * Tells the native side where every point sits, without pressing anything. Without this the pad's
   * L3/R3 would have nothing to hold down while the rings are hidden behind a controller.
   */
  private void syncTouchMapPositions()
  {
    final int width = getWidth();
    final int height = getHeight();
    if (width <= 0 || height <= 0)
      return;

    for (InputOverlayDrawableButton button : overlayButtons) {
      final int slot = touchMapSlot(button.getLegacyId());
      if (slot < 0)
        continue;
      final Rect bounds = button.getBounds();
      setMappedTouchPosition(slot, bounds.exactCenterX() / width, bounds.exactCenterY() / height);
    }
  }

  /**
   * Tells the native side which points sweep and towards which side. Repeated on every rebuild for
   * the same reason as the pad mapping: the emulator session may not have existed the first time.
   */
  private void applyTouchMapSweeps()
  {
    for (int i = 0; i < ButtonType.TOUCH_MAP_COUNT; i++) {
      final float dx = TOUCH_MAP_SWIPE_DX[i];
      final float dy = TOUCH_MAP_SWIPE_DY[i];
      setMappedTouchSwipe(i, dx, dy, (dx == 0f && dy == 0f) ? 0f : TOUCH_MAP_SWIPE_SPAN);
    }
  }

  /**
   * Initializes an {@link InputOverlayDrawableDpad}
   *
   * @param context                   The current {@link Context}.
   * @param defaultResId              The {@link Bitmap} resource ID of the default sate.
   * @param pressedOneDirectionResId  The {@link Bitmap} resource ID of the pressed sate in one direction.
   * @param pressedTwoDirectionsResId The {@link Bitmap} resource ID of the pressed sate in two directions.
   * @param legacyId                  Legacy identifier for the up button.
   * @param upControl                 Control identifier for the up button.
   * @param downControl               Control identifier for the down button.
   * @param leftControl               Control identifier for the left button.
   * @param rightControl              Control identifier for the right button.
   * @return the initialized {@link InputOverlayDrawableDpad}
   */
  private static InputOverlayDrawableDpad initializeOverlayDpad(Context context,
          int defaultResId,
          int pressedOneDirectionResId,
          int pressedTwoDirectionsResId,
          int legacyId,
          int upControl,
          int downControl,
          int leftControl,
          int rightControl,
          OverlayLayout layout,
          LayoutBounds layoutBounds,
          float globalScale,
          int globalOpacity)
  {
    // Resources handle for fetching the initial Drawable resource.
    final Resources res = context.getResources();

    // Decide scale based on button ID and user preference
    float scale = 0.35f;

    scale *= globalScale;

    // Initialize the InputOverlayDrawableDpad.
    final Bitmap defaultStateBitmap =
            resizeBitmap(context, BitmapFactory.decodeResource(res, defaultResId), scale);
    final Bitmap pressedOneDirectionStateBitmap =
            resizeBitmap(context, BitmapFactory.decodeResource(res, pressedOneDirectionResId),
                    scale);
    final Bitmap pressedTwoDirectionsStateBitmap =
            resizeBitmap(context, BitmapFactory.decodeResource(res, pressedTwoDirectionsResId),
                    scale);
    final InputOverlayDrawableDpad overlayDrawable =
            new InputOverlayDrawableDpad(res, defaultStateBitmap,
                    pressedOneDirectionStateBitmap, pressedTwoDirectionsStateBitmap,
                    legacyId, upControl, downControl, leftControl, rightControl);

    OverlayPosition position = layout.positionFor(legacyId);
    if (position == null)
      position = new OverlayPosition(0f, 0f);
    int drawableX = Math.round(position.getNormalizedX() * layoutBounds.width);
    int drawableY = Math.round(position.getNormalizedY() * layoutBounds.height);

    int width = overlayDrawable.getWidth();
    int height = overlayDrawable.getHeight();

    // Now set the bounds for the InputOverlayDrawableDpad.
    // This will dictate where on the screen (and the what the size) the InputOverlayDrawableDpad will be.
    overlayDrawable.setBounds(drawableX, drawableY, drawableX + width, drawableY + height);

    // Need to set the image's position
    overlayDrawable.setPosition(drawableX, drawableY);
    overlayDrawable.setOpacity((int) (globalOpacity * 0.01 * 255));

    return overlayDrawable;
  }

  /**
   * Initializes an {@link InputOverlayDrawableJoystick}
   *
   * @param context         The current {@link Context}
   * @param resOuter        Resource ID for the outer image of the joystick (the static image that shows the circular bounds).
   * @param defaultResInner Resource ID for the default inner image of the joystick (the one you actually move around).
   * @param pressedResInner Resource ID for the pressed inner image of the joystick.
   * @param legacyId        Legacy identifier (ButtonType) for which joystick this is.
   * @param xControl        Control identifier for the X axis.
   * @param yControl        Control identifier for the Y axis.
   * @return the initialized {@link InputOverlayDrawableJoystick}.
   */
  private static InputOverlayDrawableJoystick initializeOverlayJoystick(Context context,
          int resOuter, int defaultResInner, int pressedResInner, int legacyId, int xControl,
          int yControl, OverlayLayout layout, LayoutBounds layoutBounds, float globalScale,
          int globalOpacity)
  {
    // Resources handle for fetching the initial Drawable resource.
    final Resources res = context.getResources();

    // Decide scale based on user preference
    float scale = 0.275f;
    scale *= globalScale;

    // Initialize the InputOverlayDrawableJoystick.
    final Bitmap bitmapOuter =
            resizeBitmap(context, BitmapFactory.decodeResource(res, resOuter), scale);
    final Bitmap bitmapInnerDefault = BitmapFactory.decodeResource(res, defaultResInner);
    final Bitmap bitmapInnerPressed = BitmapFactory.decodeResource(res, pressedResInner);

    OverlayPosition position = layout.positionFor(legacyId);
    if (position == null)
      position = new OverlayPosition(0f, 0f);
    int drawableX = Math.round(position.getNormalizedX() * layoutBounds.width);
    int drawableY = Math.round(position.getNormalizedY() * layoutBounds.height);

    // Decide inner scale based on joystick ID
    float innerScale = 1.375f;

    // Now set the bounds for the InputOverlayDrawableJoystick.
    // This will dictate where on the screen (and the what the size) the InputOverlayDrawableJoystick will be.
    int outerSize = bitmapOuter.getWidth();
    Rect outerRect = new Rect(drawableX, drawableY, drawableX + outerSize, drawableY + outerSize);
    Rect innerRect = new Rect(0, 0, (int) (outerSize / innerScale), (int) (outerSize / innerScale));

    // Send the drawableId to the joystick so it can be referenced when saving control position.
    final InputOverlayDrawableJoystick overlayDrawable =
            new InputOverlayDrawableJoystick(res, bitmapOuter, bitmapInnerDefault,
                    bitmapInnerPressed, outerRect, innerRect, legacyId, xControl, yControl);

    // Need to set the image's position
    overlayDrawable.setPosition(drawableX, drawableY);
    overlayDrawable.setOpacity((int) (globalOpacity * 0.01 * 255));

    return overlayDrawable;
  }

  public void setIsInEditMode(boolean isInEditMode)
  {
    mIsInEditMode = isInEditMode;
    if (mIsInEditMode) {
      mHideOverlayButtons = false;
      mShowingOverlay = true;
    }
  }

  public boolean isPointerOwned(int pointerId)
  {
    for (InputOverlayDrawableButton button : overlayButtons)
      if (button.getTrackId() == pointerId)
        return true;
    for (InputOverlayDrawableDpad dpad : overlayDpads)
      if (dpad.getTrackId() == pointerId)
        return true;
    for (InputOverlayDrawableJoystick joystick : overlayJoysticks)
      if (joystick.getTrackId() == pointerId)
        return true;
    return false;
  }

  public boolean isInEditMode()
  {
    return mIsInEditMode;
  }

  public void setAutoHideEnabled(boolean autoHideEnabled)
  {
    mAutoHideEnabled = autoHideEnabled;
    if(!mAutoHideEnabled){
      mShowingOverlay = true;
      invalidate();
    } else {
      resetHideTimer();
    }
  }

  public void setAllowVirtualController(boolean allowVirtualController)
  {
    if(mAllowVirtualController == allowVirtualController)
      return;

    mAllowVirtualController = allowVirtualController;
    releaseInactiveInputs();
    updateVirtualControllerState();
  }

  public void setLayout(OverlayLayout layout)
  {
    setLayout(layout, true);
  }

  public OverlayLayout captureLayout()
  {
    return mLayout.normalized();
  }

  public void refreshOverlayScope(String scopeId)
  {
    boolean allowGameOverride = OverlayStore.hasGameOverride(getContext(), scopeId);
    OverlayState state = OverlayStore.resolveState(getContext(), scopeId, allowGameOverride);
    applyResolvedState(state, true);
  }

  public void rebindController()
  {
    releaseAllInputs();
    detachVirtualController();
    updateVirtualControllerState();
  }

  private void updateVirtualControllerState()
  {
    boolean shouldAttach = mAllowVirtualController && mOverlayMask != 0 && getWindowToken() != null;
    if (shouldAttach == mControllerAttached)
      return;

    if (shouldAttach) {
      attachController();
      mControllerAttached = true;
    } else {
      detachVirtualController();
    }
  }

  private void detachVirtualController()
  {
    detachController();
    mControllerAttached = false;
  }

  public native void attachController();
  public native void detachController();
  public native void setAxis(int axis, short value);
  public native void setButton(int button, boolean value);
  public native void setTouchState(int mode); // 0 = front, 1 = back, 2 = front + back mirrored
  // touch mapping: reports a finger held at a normalized position on the touch panel
  public native int setMappedTouch(int slot, float normalizedX, float normalizedY, boolean active);
  // stores where a point sits without pressing it, so a pad button can use it while rings are hidden
  public native int setMappedTouchPosition(int slot, float normalizedX, float normalizedY);
  // turns a point into a sweep: pressing it drags a finger from the screen centre towards (dx, dy)
  public native int setMappedTouchSwipe(int slot, float dx, float dy, float span);
  // lets the pad's L3/R3 hold points 1 and 2 down (the pad is polled natively, not here)
  public native void setPadTouchMapping(boolean enabled);
  public native void clearMappedTouch();

  private LayoutBounds resolveLayoutBounds() {
    return resolveLayoutBounds(getWidth(), getHeight(), getContext());
  }

  private static LayoutBounds resolveLayoutBounds(int width, int height, Context context) {
    if (width <= 0 || height <= 0) {
      DisplayMetrics metrics = context.getResources().getDisplayMetrics();
      width = metrics.widthPixels;
      height = metrics.heightPixels;
    }

    return new LayoutBounds(width, height);
  }

  private void setLayout(OverlayLayout layout, boolean refresh)
  {
    OverlayLayout normalizedLayout = layout != null ? layout.normalized()
            : OverlayStore.defaultLayout(getContext());
    if (normalizedLayout.equals(mLayout))
      return;

    mLayout = normalizedLayout;

    if (refresh)
      refreshControls();
  }

  private void applyResolvedState(OverlayState state, boolean refresh)
  {
    OverlayState resolvedState = state != null ? state : OverlayStore.defaultState(getContext());
    OverlayLayout normalizedLayout = resolvedState.getLayout().normalized();
    OverlayConfig normalizedConfig = resolvedState.getConfig().normalized();
    float resolvedScale = normalizedConfig.getOverlayScale() / 100f;
    int resolvedOpacity = normalizedConfig.getOverlayOpacity();
    boolean resolvedTouchSwitchTilt = normalizedConfig.getTouchSwitchTilt();
    boolean resolvedPadTouchMapping = normalizedConfig.getPadTouchMapping();
    boolean changed = !normalizedLayout.equals(mLayout)
            || Float.compare(mScale, resolvedScale) != 0
            || mOpacity != resolvedOpacity
            || mTouchSwitchTilt != resolvedTouchSwitchTilt
            || mPadTouchMapping != resolvedPadTouchMapping;

    mLayout = normalizedLayout;
    mScale = resolvedScale;
    mOpacity = resolvedOpacity;
    mTouchSwitchTilt = resolvedTouchSwitchTilt;
    mPadTouchMapping = resolvedPadTouchMapping;

    if (refresh && changed)
      refreshControls();
  }

  private static final class LayoutBounds {
    final int width;
    final int height;

    LayoutBounds(int width, int height) {
      this.width = width;
      this.height = height;
    }
  }

  public static final class ButtonType
  {
    public static final int BUTTON_CROSS = 0;
    public static final int BUTTON_CIRCLE = 1;
    public static final int BUTTON_SQUARE = 2;
    public static final int BUTTON_TRIANGLE = 3;
    public static final int BUTTON_SELECT = 4;
    public static final int BUTTON_START = 5;
    public static final int DPAD_UP = 6;
    public static final int BUTTON_PS = 7;
    public static final int STICK_LEFT = 10;
    public static final int STICK_RIGHT = 15;
    public static final int TRIGGER_L = 20;
    public static final int TRIGGER_R = 21;
    public static final int TRIGGER_L2 = 22;
    public static final int TRIGGER_R2 = 23;
    public static final int TRIGGER_L3 = 24;
    public static final int TRIGGER_R3 = 25;
    public static final int BUTTON_TOUCH_HIDE = 1023;
    public static final int BUTTON_TOUCH_SWITCH = 1024;
    // touch mapping points (also used as the layout key of each point)
    public static final int TOUCH_MAP_1 = 1100;
    public static final int TOUCH_MAP_2 = 1101;
    public static final int TOUCH_MAP_3 = 1102;
    public static final int TOUCH_MAP_4 = 1103;
    public static final int TOUCH_MAP_COUNT = 4;
  }

  // SDL values
  public static class ControlId {
    public static final int a = 0;
    public static final int b = 1;
    public static final int x = 2;
    public static final int y = 3;
    public static final int select = 4;
    public static final int guide = 5;
    public static final int start = 6;
    public static final int l1 = 9;
    public static final int r1 = 10;
    public static final int dup = 11;
    public static final int ddown = 12;
    public static final int dleft = 13;
    public static final int dright = 14;

    public static final int l3 = 7;
    public static final int r3 = 8;

    // they are axis for sdl but buttons for the ps vita
    public static final int l2 = -4;
    public static final int r2 = -5;

    // button to switch between front and back touch
    public static final int touch = 1024;

    public static final int axis_left_x = 0;
    public static final int axis_left_y = 1;
    public static final int axis_right_x = 2;
    public static final int axis_right_y = 3;
  }
}
