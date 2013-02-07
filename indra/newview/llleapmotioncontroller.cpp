/** 
 * @file llleapmotioncontroller.cpp
 * @brief LLLeapMotionController class implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public 
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/* 
	This is experimental code the connects the Leap Motion gesture controller (www.leapmotion.com) 
	to the Second Life viewer.   
	
	To set up your source repo to compile, you need the Leap Motion SDK from www.leapmotion.com.
	Linden Lab does not provide this code.

	*	The Leap Motion SDK should be copied to the same folder as the root of the 
		Second Life viewer source code, otherwise the #include line below will fail.

	*	Copy Leap.lib from Leap_SDK\lib\x86 into the build-vc100\packages\lib\release directory, otherwise you 
		will get linker errors not finding leap.lib.
	
	*   Copy Leap.dll from Leap_SDK\lib\x86 into the build-vc100\newview\RelWithDebInfo directory, otherwise 
		secondlife-bin.exe will not run

	This code uses the value of "LeapmotionTestMode" from \indra\newview\app_settings to determine how
	it functions.   You can change this under the Advanced menu, Show Debug Settings.  Enter or find 
	"LeapmotionTestMode" mode in the Debug Settings and change the value as appropriate.
	
	With this experimental code, "LeapmotionTestMode" can be set to operate as follows:

		0 : control flying
		1 : send hand and finger data to the Second Life world via back-channel chat for scripts to detect and use
		2 : basic hand motion detection that triggers an avatar gesture
		3 : control AV movement and camera
		411 : dump raw information from the controller to the viewer log
	
	See the LLLMImpl::stepFrame() function below to follow the logic.

	For future work, check out LLJoystick* classes and how those devices work with Second Life.
*/


#include "llviewerprecompiledheaders.h"

#include "llleapmotioncontroller.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llgesturemgr.h"
#include "llmath.h"
//#include "llnearbychatbar.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewercontrol.h"

// The Leapmotion SDK should be copied to the same folder as the root of
// the Second Life viewer source code.  Otherwise you must modify this line to find Leap.h
#include "../../../../../Leap_SDK/include/Leap.h"



const F32	LM_DEAD_ZONE			= 20.f;		// Dead zone in the middle of the space
const F32   LM_ORBIT_RATE_FACTOR	= 80.f;		// Number for camera orbit magic factor



class 	LLLMImpl : public Leap::Listener
{
public:
	LLLMImpl();
	~LLLMImpl();

	// LeapMotion callbacks
	virtual void onInit(const Leap::Controller&);
    virtual void onConnect(const Leap::Controller&);
    virtual void onDisconnect(const Leap::Controller&);
    virtual void onFrame(const Leap::Controller&);

	// Called from viewer main loop
	void	stepFrame();

	Leap::Controller * mLMController;		// Leapmotion's object
	bool			mLMConnected;			// true if device is connected
	bool			mFrameAvailable;		// true if there is a new frame of data available
	int64_t			mCurrentFrameID;		// Id of the most recent frame of data

	LLTimer			mYawTimer;				// Avoid turning left / right too fast

	// Hacky demo code - send controller data to in-world objects via chat
	LLTimer			mChatMsgTimer;			// Throttle sending LM controller data to region local chat

	LLTimer			mGestureTimer;			// Throttle invoking SL gestures

private:

	// Various controller modes
	void	modeFlyingControlTest(Leap::HandList & hands);
	void	modeStreamDataToSL(Leap::HandList & hands);
	void	modeGestureDetection1(Leap::HandList & hands);
	void	modeMoveAndCamTest1(Leap::HandList & hands);
	void	modeDumpDebugInfo(Leap::HandList & hands);
};

const F32		LLLEAP_YAW_INTERVAL = 0.075f;

// Time between spamming chat messages.  Server-side throttle is 200 msgs in 10 seconds
const F32		LLLEAP_CHAT_MSG_INTERVAL = 0.200f;		// Send 5/second

const F32		LLLEAP_GESTURE_INTERVAL = 3.f;		// 3 seconds in between SL gestures


LLLMImpl::LLLMImpl() : mLMController(NULL),
						mLMConnected(false),
						mFrameAvailable(false),
						mCurrentFrameID(0)
{
	mLMController = new Leap::Controller(*this);
	mYawTimer.setTimerExpirySec(LLLEAP_YAW_INTERVAL);
	mChatMsgTimer.setTimerExpirySec(LLLEAP_CHAT_MSG_INTERVAL);
	mGestureTimer.setTimerExpirySec(LLLEAP_GESTURE_INTERVAL);
}


LLLMImpl::~LLLMImpl()
{
	delete mLMController;
}


void LLLMImpl::onInit(const Leap::Controller& controller) 
{
	llinfos << "Initialized" << llendl;
}

void LLLMImpl::onConnect(const Leap::Controller& controller) 
{
	llinfos << "Connected" << llendl;
	mLMConnected = true;
	mCurrentFrameID = 0;
}

void LLLMImpl::onDisconnect(const Leap::Controller& controller) 
{
	llinfos << "Disconnected" << llendl;
	mLMConnected = false;
}


// Callback from Leapmotion code when a new frame is available.  It simply
// sets a flag so stepFrame() can pick up new controller data
void LLLMImpl::onFrame(const Leap::Controller& controller) 
{
	if (mLMConnected)
	{
		// Get the most recent frame and report some basic information
		const Leap::Frame frame = controller.frame();
		int64_t frame_id = frame.id();
		if (frame_id != mCurrentFrameID)
		{	// Just record the ID and set flag indicating data is available
			mCurrentFrameID = frame_id;
			mFrameAvailable = true;
		}
	}
}


// This is called every SL viewer frame
void LLLMImpl::stepFrame()
{
	if (mLMController &&
		mFrameAvailable &&
		mLMConnected)
	{
		mFrameAvailable = false;

		// Get the most recent frame and report some basic information
		const Leap::Frame frame = mLMController->frame();
		Leap::HandList hands = frame.hands();
		
		S32 controller_mode = gSavedSettings.getS32("LeapmotionTestMode");

		if (controller_mode == 0)
		{
			modeFlyingControlTest(hands);
		}
		else if (controller_mode == 1)
		{
			modeStreamDataToSL(hands);
		}
		else if (controller_mode == 2)
		{	// Click detection
			modeGestureDetection1(hands);
		}
		else if (controller_mode == 3)
		{	// Movement and camera control
			modeMoveAndCamTest1(hands);
		}
		else if (controller_mode == 411)
		{			// Dump out data
			modeDumpDebugInfo(hands);
		}	// dump_out_data
	}
}

// This controller mode is used to fly the avatar, going up, down, forward and turning.
void LLLMImpl::modeFlyingControlTest(Leap::HandList & hands)
{
	static S32 sLMFlyingHysteresis = 0;

	S32 numHands = hands.count();		
	bool agent_is_flying =  (bool) gAgent.getFlying();

	if (numHands == 0 &&
		agent_is_flying &&
		sLMFlyingHysteresis > 0)
	{
		sLMFlyingHysteresis--;
		if (sLMFlyingHysteresis == 0)
		{
			llinfos << "LM stop flying - look ma, no hands!" << llendl;
			gAgent.setFlying(FALSE);
		}
	}
	else if (numHands == 1)
	{
		// Get the first hand
		Leap::Hand hand = hands[0];

		// Check if the hand has any fingers
		Leap::FingerList finger_list = hand.fingers();
		S32 num_fingers = finger_list.count();

		Leap::Vector palm_pos = hand.palmPosition();
		Leap::Vector palm_normal = hand.palmNormal();

		F32 ball_radius = (F32) hand.sphereRadius();
		Leap::Vector ball_center = hand.sphereCenter();

		// Number of fingers controls flying on / off
		if (num_fingers == 0 &&			// To do - add hysteresis or data smoothing?
			agent_is_flying)
		{
			if (sLMFlyingHysteresis > 0)
			{
				sLMFlyingHysteresis--;
			}
			else
			{
				llinfos << "LM stop flying" << llendl;
				gAgent.setFlying(FALSE);
			}
		}
		else if (num_fingers > 2 && 
				!agent_is_flying)
		{
			llinfos << "LM start flying" << llendl;
			gAgent.setFlying(TRUE);
			sLMFlyingHysteresis = 5;
		}

		// Radius of ball controls forward motion
		if (agent_is_flying)
		{

			if (ball_radius > 110.f)
			{	// Open hand, move fast
				gAgent.setControlFlags(AGENT_CONTROL_AT_POS | AGENT_CONTROL_FAST_AT);
			}
			else if (ball_radius > 85.f)
			{	// Partially open, move slow
				gAgent.setControlFlags(AGENT_CONTROL_AT_POS);
			}
			else
			{	// Closed - stop
				gAgent.clearControlFlags(AGENT_CONTROL_AT_POS);
			}

			// Height of palm controls moving up and down
			if (palm_pos.y > 260.f)
			{	// Go up fast
				gAgent.setControlFlags(AGENT_CONTROL_UP_POS | AGENT_CONTROL_FAST_UP);
			}
			else if (palm_pos.y > 200.f)
			{	// Go up
				gAgent.setControlFlags(AGENT_CONTROL_UP_POS);
			}
			else if (palm_pos.y < 60.f)
			{	// Go down fast
				gAgent.setControlFlags(AGENT_CONTROL_FAST_UP | AGENT_CONTROL_UP_NEG);
			}
			else if (palm_pos.y < 120.f)
			{	// Go down
				gAgent.setControlFlags(AGENT_CONTROL_UP_NEG);
			}
			else
			{	// Clear up / down
				gAgent.clearControlFlags(AGENT_CONTROL_FAST_UP | AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG);
			}

			// Palm normal going left / right controls direction
			if (mYawTimer.checkExpirationAndReset(LLLEAP_YAW_INTERVAL))
			{
				if (palm_normal.x > 0.4)
				{	// Go left fast
					gAgent.moveYaw(1.f);
				}
				else if (palm_normal.x < -0.4)
				{	// Go right fast
					gAgent.moveYaw(-1.f);
				}
			}

		}		// end flying controls
	}
}


// This experimental mode sends chat messages into SL on a back channel for LSL scripts
// to intercept with a listen() event.   This is experimental and not sustainable for
// a production feature ... many avatars using this would flood the chat system and
// hurt server performance.   Depending on how useful this proves to be, a better
// mechanism should be designed to stream data from the viewer into SL scripts.
void LLLMImpl::modeStreamDataToSL(Leap::HandList & hands)
{
	S32 numHands = hands.count();
	if (numHands == 1 &&
		mChatMsgTimer.checkExpirationAndReset(LLLEAP_CHAT_MSG_INTERVAL))
	{
		// Get the first (and only) hand
		Leap::Hand hand = hands[0];

		Leap::Vector palm_pos = hand.palmPosition();
		Leap::Vector palm_normal = hand.palmNormal();

		F32 ball_radius = (F32) hand.sphereRadius();
		Leap::Vector ball_center = hand.sphereCenter();

		// Chat message looks like "/2343 LM1,<palm pos>,<palm normal>,<sphere center>,<sphere radius>"
		LLVector3 vec;
		std::stringstream status_chat_msg;
		status_chat_msg << "/2343 LM,";
		status_chat_msg << "<" << palm_pos.x << "," << palm_pos.y << "," << palm_pos.z << ">,";
		status_chat_msg << "<" << palm_normal.x << "," << palm_normal.y << "," << palm_normal.z << ">,";
		status_chat_msg << "<" << ball_center.x << "," << ball_center.y << "," << ball_center.z << ">," << ball_radius;

		//LLNearbyChatBar::sendChatFromViewer(status_chat_msg.str(), CHAT_TYPE_SHOUT, FALSE);
	}
}

// This mode tries to detect simple hand motion and either triggers an avatar gesture or 
// sends a chat message into SL in response.   It is very rough, hard-coded for detecting 
// a hand wave (a SL gesture) or the wiggling-thumb gun trigger (a chat message sent to a
// special version of the popgun).
void LLLMImpl::modeGestureDetection1(Leap::HandList & hands)
{
	static S32 trigger_direction = -1;

	S32 numHands = hands.count();
	if (numHands == 1)
	{
		// Get the first hand
		Leap::Hand hand = hands[0];

		// Check if the hand has any fingers
		Leap::FingerList finger_list = hand.fingers();
		S32 num_fingers = finger_list.count();
		static S32 last_num_fingers = 0;

		if (num_fingers == 1)
		{	// One finger ... possibly reset the 
			Leap::Finger finger = finger_list[0];
			Leap::Vector finger_dir = finger.direction();

			// Negative Z is into the screen - check that it's the largest component
			S32 abs_z_dir = llabs(finger_dir.z);
			if (finger_dir.z < -0.5 &&
				abs_z_dir > llabs(finger_dir.x) &&
				abs_z_dir > llabs(finger_dir.y))
			{
				Leap::Vector finger_pos = finger.tipPosition();
				Leap::Vector finger_vel = finger.tipVelocity(); 
				llinfos << "finger direction is " << finger_dir.x << ", " << finger_dir.y << ", " << finger_dir.z
					<< ", position " << finger_pos.x << ", " << finger_pos.y << ", " << finger_pos.z 
					<< ", velocity " << finger_vel.x << ", " << finger_vel.y << ", " << finger_vel.z 
					<< llendl;
			}

			if (trigger_direction != -1)
			{
				llinfos << "Reset trigger_direction - one finger" << llendl;
				trigger_direction = -1;
			}
		}
		else if (num_fingers == 2)
		{
			Leap::Finger barrel_finger = finger_list[0];
			Leap::Vector barrel_finger_dir = barrel_finger.direction();

			// Negative Z is into the screen - check that it's the largest component
			F32 abs_z_dir = llabs(barrel_finger_dir.z);
			if (barrel_finger_dir.z < -0.5f &&
				abs_z_dir > llabs(barrel_finger_dir.x) &&
				abs_z_dir > llabs(barrel_finger_dir.y))
			{
				Leap::Finger thumb_finger = finger_list[1];
				Leap::Vector thumb_finger_dir = thumb_finger.direction();
				Leap::Vector thumb_finger_pos = thumb_finger.tipPosition();
				Leap::Vector thumb_finger_vel = thumb_finger.tipVelocity();

				if ((thumb_finger_dir.x < barrel_finger_dir.x) )
				{	// Trigger gunfire
					if (trigger_direction < 0 &&		// Haven't fired
						thumb_finger_vel.x > 50.f &&	// Moving into screen
						thumb_finger_vel.z < -50.f &&
						mChatMsgTimer.checkExpirationAndReset(LLLEAP_CHAT_MSG_INTERVAL))
					{
						// Chat message looks like "/2343 LM2 gunfire"
						std::string gesture_chat_msg("/2343 LM2 gunfire");
						//LLNearbyChatBar::sendChatFromViewer(gesture_chat_msg, CHAT_TYPE_SHOUT, FALSE);
						trigger_direction = 1;
						llinfos << "Sent gunfire chat" << llendl;
					}
					else if (trigger_direction > 0 &&	// Have fired, need to pull thumb back
						thumb_finger_vel.x < -50.f &&
						thumb_finger_vel.z > 50.f)		// Moving out of screen
					{
						trigger_direction = -1;
						llinfos << "Reset trigger_direction" << llendl;
					}
				}
			}
			else if (trigger_direction != -1)
			{
				llinfos << "Reset trigger_direction - hand pos" << llendl;
				trigger_direction = -1;
			}
		}
		else if (num_fingers == 5 &&
			num_fingers == last_num_fingers)
		{
			if (mGestureTimer.checkExpirationAndReset(LLLEAP_GESTURE_INTERVAL))
			{
				// figure out a gesture to trigger
				std::string gestureString("/overhere");
				LLGestureMgr::instance().triggerAndReviseString( gestureString );
			}
		}
		
		last_num_fingers = num_fingers;
	}
}


// This mode tries to move the avatar and camera in Second Life.   It's pretty rough and needs a lot of work
void LLLMImpl::modeMoveAndCamTest1(Leap::HandList & hands)
{
	S32 numHands = hands.count();		
	if (numHands == 1)
	{
		// Get the first hand
		Leap::Hand hand = hands[0];

		// Check if the hand has any fingers
		Leap::FingerList finger_list = hand.fingers();
		S32 num_fingers = finger_list.count();

		F32 orbit_rate = 0.f;

		Leap::Vector pos(0, 0, 0);
		for (size_t i = 0; i < num_fingers; ++i) 
		{
			Leap::Finger finger = finger_list[i];
			pos += finger.tipPosition();
		}
		pos = Leap::Vector(pos.x/num_fingers, pos.y/num_fingers, pos.z/num_fingers);

		if (num_fingers == 1)
		{	// 1 finger - move avatar
			if (pos.x < -LM_DEAD_ZONE)
			{	// Move left
				gAgent.moveLeftNudge(1.f);
			}
			else if (pos.x > LM_DEAD_ZONE)
			{
				gAgent.moveLeftNudge(-1.f);
			}
			
			/*
			if (pos.z < -LM_DEAD_ZONE)
			{
				gAgent.moveAtNudge(1.f);
			}
			else if (pos.z > LM_DEAD_ZONE)
			{	
				gAgent.moveAtNudge(-1.f);
			} */

			if (pos.y < -LM_DEAD_ZONE)
			{
				gAgent.moveYaw(-1.f);
			}
			else if (pos.y > LM_DEAD_ZONE)
			{
				gAgent.moveYaw(1.f);
			}
		}	// end 1 finger
		else if (num_fingers == 2)
		{	// 2 fingers - move camera around
			// X values run from about -170 to +170
			if (pos.x < -LM_DEAD_ZONE)
			{	// Camera rotate left
				gAgentCamera.unlockView();
				orbit_rate = (llabs(pos.x) - LM_DEAD_ZONE) / LM_ORBIT_RATE_FACTOR;
				gAgentCamera.setOrbitLeftKey(orbit_rate);
			}
			else if (pos.x > LM_DEAD_ZONE)
			{
				gAgentCamera.unlockView();
				orbit_rate = (pos.x - LM_DEAD_ZONE) / LM_ORBIT_RATE_FACTOR;
				gAgentCamera.setOrbitRightKey(orbit_rate);
			}
			if (pos.z < -LM_DEAD_ZONE)
			{	// Camera zoom in
				gAgentCamera.unlockView();
				orbit_rate = (llabs(pos.z) - LM_DEAD_ZONE) / LM_ORBIT_RATE_FACTOR;
				gAgentCamera.setOrbitInKey(orbit_rate);
			}
			else if (pos.z > LM_DEAD_ZONE)
			{	// Camera zoom out
				gAgentCamera.unlockView();
				orbit_rate = (pos.z - LM_DEAD_ZONE) / LM_ORBIT_RATE_FACTOR;
				gAgentCamera.setOrbitOutKey(orbit_rate);
			}

			if (pos.y < -LM_DEAD_ZONE)
			{	// Camera zoom in
				gAgentCamera.unlockView();
				orbit_rate = (llabs(pos.y) - LM_DEAD_ZONE) / LM_ORBIT_RATE_FACTOR;
				gAgentCamera.setOrbitUpKey(orbit_rate);
			}
			else if (pos.y > LM_DEAD_ZONE)
			{	// Camera zoom out
				gAgentCamera.unlockView();
				orbit_rate = (pos.y - LM_DEAD_ZONE) / LM_ORBIT_RATE_FACTOR;
				gAgentCamera.setOrbitDownKey(orbit_rate);
			}
		}	// end 2 finger
	}
}


// This controller mode just dumps out a bunch of the Leap Motion device data, which can then be
// analyzed for other use.
void LLLMImpl::modeDumpDebugInfo(Leap::HandList & hands)
{
	S32 numHands = hands.count();		
	if (numHands == 1)
	{
		// Get the first hand
		Leap::Hand hand = hands[0];

		// Check if the hand has any fingers
		Leap::FingerList finger_list = hand.fingers();
		S32 num_fingers = finger_list.count();

		if (num_fingers >= 1) 
		{	// Calculate the hand's average finger tip position
			Leap::Vector pos(0, 0, 0);
			Leap::Vector direction(0, 0, 0);
			for (size_t i = 0; i < num_fingers; ++i) 
			{
				Leap::Finger finger = finger_list[i];
				pos += finger.tipPosition();
				direction += finger.direction();

				// Lots of log spam
				llinfos << "Finger " << i << " string is " << finger.toString() << llendl;
			}
			pos = Leap::Vector(pos.x/num_fingers, pos.y/num_fingers, pos.z/num_fingers);
			direction = Leap::Vector(direction.x/num_fingers, direction.y/num_fingers, direction.z/num_fingers);

			llinfos << "Hand has " << num_fingers << " fingers with average tip position"
				<< " (" << pos.x << ", " << pos.y << ", " << pos.z << ")" 
				<< " direction (" << direction.x << ", " << direction.y << ", " << direction.z << ")" 
				<< llendl;

		}

		Leap::Vector palm_pos = hand.palmPosition();
		Leap::Vector palm_normal = hand.palmNormal();
		llinfos << "Palm pos " << palm_pos.x
			<< ", " <<  palm_pos.y
			<< ", " <<  palm_pos.z
			<< ".   Normal: " << palm_normal.x
			<< ", " << palm_normal.y
			<< ", " << palm_normal.z
			<< llendl;

		F32 ball_radius = (F32) hand.sphereRadius();
		Leap::Vector ball_center = hand.sphereCenter();
		llinfos << "Ball pos " << ball_center.x
			<< ", " << ball_center.y
			<< ", " << ball_center.z
			<< ", radius " << ball_radius
			<< llendl;
	}	// dump_out_data
}


// --------------------------------------------------------------------------------
// The LLLeapMotionController class is a thin public glue layer into the LLLMImpl
// class, which does all the interesting work.

// One controller instance to rule them all
LLLeapMotionController::LLLeapMotionController()
{
	mController = new LLLMImpl();
}


LLLeapMotionController::~LLLeapMotionController()
{
	delete mController;
	mController = NULL;
}


// Called every viewer frame
void LLLeapMotionController::stepFrame()
{
	if (mController &&
		STATE_STARTED == LLStartUp::getStartupState())
	{
		mController->stepFrame();
	}
}
