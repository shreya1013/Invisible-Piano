import cv2
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
import urllib.request
import numpy as np
import serial
import time
import os

# ---- CHANGE THIS TO YOUR XIAO'S IP ADDRESS ---- # home IP Address: http://10.0.0.201:81/stream
STREAM_URL = "http://10.44.61.212:81/stream"  
# ------------------------------------------------

# ---- CHANGE THIS TO YOUR ESP32'S COM PORT ----
ser = serial.Serial('COM3', 115200, timeout=1)
time.sleep(2)
print("Serial connected to ESP32")
# -----------------------------------------------

if not os.path.exists("hand_landmarker.task"):
    print("Downloading hand landmarker model...")
    urllib.request.urlretrieve(
        "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task",
        "hand_landmarker.task"
    )
    print("Model downloaded!")

base_options = python.BaseOptions(model_asset_path="hand_landmarker.task")
options = vision.HandLandmarkerOptions(
    base_options=base_options,
    num_hands=2,
    min_hand_detection_confidence=0.7,
    min_tracking_confidence=0.7
)
detector = vision.HandLandmarker.create_from_options(options)

cap = cv2.VideoCapture(STREAM_URL)

if not cap.isOpened():
    print("ERROR: Could not connect to camera stream")
    exit()

print("Connected to camera stream successfully!")

def is_finger_down(landmarks, tip_id, pip_id):
    return landmarks[tip_id].y > landmarks[pip_id].y

def draw_landmarks(frame, landmarks):
    h, w = frame.shape[:2]
    for lm in landmarks:
        cx, cy = int(lm.x * w), int(lm.y * h)
        cv2.circle(frame, (cx, cy), 4, (0, 255, 0), -1)
    connections = mp.tasks.vision.HandLandmarksConnections.HAND_CONNECTIONS
    for connection in connections:
        start = landmarks[connection.start]
        end = landmarks[connection.end]
        x1, y1 = int(start.x * w), int(start.y * h)
        x2, y2 = int(end.x * w), int(end.y * h)
        cv2.line(frame, (x1, y1), (x2, y2), (255, 255, 255), 2)

while True:
    ret, frame = cap.read()
    if not ret:
        print("Failed to grab frame")
        break

    frame = cv2.flip(frame, 1)

    rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)

    results = detector.detect(mp_image)

    right_fingers = [False, False, False, False, False]
    bpm = None

    if results.hand_landmarks and results.handedness:
        for landmarks, handedness in zip(results.hand_landmarks, results.handedness):

            draw_landmarks(frame, landmarks)

            label = handedness[0].category_name

            if label == "Right":
                right_fingers[0] = is_finger_down(landmarks, 4, 3)
                right_fingers[1] = is_finger_down(landmarks, 8, 6)
                right_fingers[2] = is_finger_down(landmarks, 12, 10)
                right_fingers[3] = is_finger_down(landmarks, 16, 14)
                right_fingers[4] = is_finger_down(landmarks, 20, 18)

            if label == "Left":
                wrist_y = landmarks[0].y
                bpm = int(200 - (wrist_y * 160))
                bpm = max(40, min(200, bpm))

    finger_str = ''.join(['1' if f else '0' for f in right_fingers])
    print(f"Fingers: {finger_str} | BPM: {bpm}")

    if bpm is not None:
        message = f"F:{finger_str},BPM:{bpm}\n"
        ser.write(message.encode())

    cv2.putText(frame, f"Fingers: {finger_str}", (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
    if bpm:
        cv2.putText(frame, f"BPM: {bpm}", (10, 70),
                    cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 255), 2)

    cv2.imshow("Invisible Piano - Hand Detection", frame)

    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()