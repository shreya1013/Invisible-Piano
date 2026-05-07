import cv2
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
import urllib.request
import numpy as np
import serial
import time
import os
import tkinter as tk
from tkinter import font as tkfont
from PIL import Image, ImageTk

# ── CONFIGURATION ─────────────────────────────────────────────────────────────
STREAM_URL  = "http://10.44.61.132:81/stream"  # replace with your ESP32 camera stream URL
COM_PORT    = "COM6"
BAUD_RATE   = 115200
current_bpm = 90   # default BPM, should match ESP32 initial value
# ──────────────────────────────────────────────────────────────────────────────

# ── Scale definitions ─────────────────────────────────────────────────────────
SCALES = {
    "C major":  ["C4",  "D4",  "E4",  "F4",  "G4"],
    "G major":  ["G4",  "A4",  "B4",  "C5",  "D5"],
    "D major":  ["D4",  "E4",  "F#4", "G4",  "A4"],
    "A major":  ["A4",  "B4",  "C#5", "D5",  "E5"],
    "F major":  ["F4",  "G4",  "A4",  "Bb4", "C5"],
    "Bb major": ["Bb4", "C5",  "D5",  "Eb5", "F5"],
}
SCALE_KEYS   = list(SCALES.keys())
FINGER_NAMES = ["Thumb", "Index", "Middle", "Ring", "Pinky"]

WHITE_KEY_NOTES = ["C4","D4","E4","F4","G4","A4","B4",
                   "C5","D5","E5","F5","G5","A5","B5"]
BLACK_KEY_AFTER = {0,1,3,4,5, 7,8,10,11,12}
ENHARMONIC = {
    "Bb4":"A#4","A#4":"Bb4",
    "Eb5":"D#5","D#5":"Eb5",
    "F#4":"Gb4","Gb4":"F#4",
    "C#5":"Db5","Db5":"C#5",
}
BLACK_KEY_NOTE_TO_WHITE_IDX = {
    "C#4":0,"Db4":0, "D#4":1,"Eb4":1,
    "F#4":3,"Gb4":3, "G#4":4,"Ab4":4,
    "A#4":5,"Bb4":5, "C#5":7,"Db5":7,
    "D#5":8,"Eb5":8, "F#5":10,"Gb5":10,
    "G#5":11,"Ab5":11, "A#5":12,"Bb5":12,
}

def note_matches_white_key(note, white_key_note):
    return note == white_key_note or ENHARMONIC.get(note) == white_key_note

def is_black_note(note):
    return note in BLACK_KEY_NOTE_TO_WHITE_IDX

# ── Colors ────────────────────────────────────────────────────────────────────
BG              = "#0f0f0f"
PANEL_BG        = "#1a1a1a"
BORDER          = "#2a2a2a"
LED_ON          = "#e24b4a"
LED_OFF         = "#2a2a2a"
TEXT_PRI        = "#f0f0f0"
TEXT_SEC        = "#888888"
BPM_COLOR       = "#378ADD"
GREEN           = "#1D9E75"
GREEN_DARK      = "#085041"
KEY_WHITE       = "#e8e8e8"
KEY_BLACK       = "#1a1a1a"
KEY_ACTIVE      = "#1D9E75"
KEY_ACTIVE_TEXT = "#E1F5EE"
KEY_MAPPED      = "#a8dfc9"
KEY_PLAYING     = "#EF9F27"
BTN_ACTIVE      = "#1D9E75"
BTN_ACTIVE_FG   = "#E1F5EE"

# ── Serial setup ──────────────────────────────────────────────────────────────
try:
    ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=1)
    time.sleep(2)
    serial_connected = True
    print("Serial connected to ESP32")
except Exception as e:
    ser = None
    serial_connected = False
    print(f"Serial not available: {e}")

# ── Model download ────────────────────────────────────────────────────────────
if not os.path.exists("hand_landmarker.task"):
    print("Downloading hand landmarker model...")
    urllib.request.urlretrieve(
        "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task",
        "hand_landmarker.task"
    )
    print("Model downloaded!")

# ── MediaPipe setup ───────────────────────────────────────────────────────────
base_options = python.BaseOptions(model_asset_path="hand_landmarker.task")
options = vision.HandLandmarkerOptions(
    base_options=base_options,
    num_hands=2,
    min_hand_detection_confidence=0.7,
    min_tracking_confidence=0.7
)
detector = vision.HandLandmarker.create_from_options(options)

# ── Camera setup ──────────────────────────────────────────────────────────────
print("Connecting to camera stream...")
cap = None
for attempt in range(10):
    cap = cv2.VideoCapture(STREAM_URL)
    if cap.isOpened():
        print(f"Connected on attempt {attempt + 1}")
        break
    print(f"Attempt {attempt + 1} failed, retrying in 2s...")
    time.sleep(2)

if cap is None or not cap.isOpened():
    print("ERROR: Could not connect to camera stream after 10 attempts")
    exit()

# ── MediaPipe helpers ─────────────────────────────────────────────────────────
def is_finger_down(landmarks, tip_id, pip_id):
    return landmarks[tip_id].y > landmarks[pip_id].y

def draw_landmarks(frame, landmarks):
    h, w = frame.shape[:2]
    for lm in landmarks:
        cx, cy = int(lm.x * w), int(lm.y * h)
        cv2.circle(frame, (cx, cy), 4, (0, 255, 0), -1)
    for connection in mp.tasks.vision.HandLandmarksConnections.HAND_CONNECTIONS:
        start = landmarks[connection.start]
        end   = landmarks[connection.end]
        cv2.line(frame,
                 (int(start.x * w), int(start.y * h)),
                 (int(end.x * w),   int(end.y * h)),
                 (255, 255, 255), 2)

# ── Alternating note state ────────────────────────────────────────────────────
alt_state = {
    "active_fingers": [],
    "sequence":       [],
    "seq_pos":        0,
    "direction":      1,
    "last_tick":      0.0,
    "interval":       0.5,
}

def build_bounce_sequence(active_finger_indices):
    n = len(active_finger_indices)
    if n <= 1:
        return active_finger_indices[:]
    up   = list(range(n))
    down = list(range(n - 2, 0, -1))
    return up + down

def update_alt_state(new_fingers):
    prev = alt_state["active_fingers"]
    if sorted(new_fingers) == sorted(prev):
        return
    alt_state["active_fingers"] = sorted(new_fingers)
    alt_state["sequence"]  = build_bounce_sequence(alt_state["active_fingers"])
    alt_state["seq_pos"]   = 0
    alt_state["direction"] = 1
    alt_state["last_tick"] = time.time()

def current_playing_finger():
    af  = alt_state["active_fingers"]
    seq = alt_state["sequence"]
    if not af or not seq:
        return None
    alt_state["seq_pos"] = alt_state["seq_pos"] % len(seq)
    pos     = alt_state["seq_pos"]
    seq_val = seq[pos]
    if seq_val >= len(af):
        seq_val = len(af) - 1
    return af[seq_val]

def advance_alt_note(now, bpm):
    seq = alt_state["sequence"]
    if len(seq) <= 1:
        return
    interval = 60.0 / max(40, bpm)
    if now - alt_state["last_tick"] >= interval:
        alt_state["seq_pos"] = (alt_state["seq_pos"] + 1) % len(seq)
        alt_state["last_tick"] = now

# ── Root window ───────────────────────────────────────────────────────────────
root = tk.Tk()
root.title("Invisible Piano — Hand Detection")
root.configure(bg=BG)
root.resizable(False, False)

mono      = tkfont.Font(family="Courier New", size=12, weight="bold")
large_fnt = tkfont.Font(family="Courier New", size=30, weight="bold")
small_fnt = tkfont.Font(family="Courier New", size=10)
lbl_fnt   = tkfont.Font(family="Courier New", size=11)
note_fnt  = tkfont.Font(family="Courier New", size=9)
btn_fnt   = tkfont.Font(family="Courier New", size=10, weight="bold")

current_scale_var = tk.StringVar(value="C major")

def current_notes():
    return SCALES[current_scale_var.get()]

# ── Layout ────────────────────────────────────────────────────────────────────
top_frame = tk.Frame(root, bg=PANEL_BG, highlightthickness=1,
                      highlightbackground=BORDER)
top_frame.grid(row=0, column=0, columnspan=2,
               padx=16, pady=(16,6), sticky="ew")

left_frame = tk.Frame(root, bg=BG)
left_frame.grid(row=1, column=0, padx=(16,8), pady=(6,16), sticky="n")

right_frame = tk.Frame(root, bg=PANEL_BG, highlightthickness=1,
                        highlightbackground=BORDER)
right_frame.grid(row=1, column=1, padx=(8,16), pady=(6,16), sticky="n")

# ── Key selector ──────────────────────────────────────────────────────────────
tk.Label(top_frame, text="Select key", font=small_fnt,
         bg=PANEL_BG, fg=TEXT_SEC).grid(row=0, column=0, padx=(14,10), pady=10)

scale_buttons = {}

def select_scale(key_name):
    current_scale_var.set(key_name)
    for name, btn in scale_buttons.items():
        btn.config(bg=BTN_ACTIVE if name == key_name else PANEL_BG,
                   fg=BTN_ACTIVE_FG if name == key_name else TEXT_PRI)
    notes = SCALES[key_name]
    for i, lbl in enumerate(note_labels):
        lbl.config(text=notes[i])
    build_piano()

for col, key_name in enumerate(SCALE_KEYS):
    display = key_name.replace("b", "\u266d")
    btn = tk.Button(
        top_frame, text=display, font=btn_fnt,
        bg=BTN_ACTIVE if key_name == "C major" else PANEL_BG,
        fg=BTN_ACTIVE_FG if key_name == "C major" else TEXT_PRI,
        activebackground=GREEN, activeforeground=BTN_ACTIVE_FG,
        relief="flat", bd=0, padx=14, pady=8, cursor="hand2",
        command=lambda k=key_name: select_scale(k)
    )
    btn.grid(row=0, column=col+1, padx=4, pady=8)
    scale_buttons[key_name] = btn

# ── Camera canvas ─────────────────────────────────────────────────────────────
CAM_W, CAM_H = 480, 320
cam_canvas = tk.Canvas(left_frame, width=CAM_W, height=CAM_H,
                        bg="#000000", highlightthickness=1,
                        highlightbackground=BORDER)
cam_canvas.pack()

finger_label = tk.Label(left_frame, text="Fingers:  0  0  0  0  0",
                         font=mono, bg=BG, fg=GREEN)
finger_label.pack(pady=(8,4))

playing_label = tk.Label(left_frame, text="Playing:  —",
                          font=mono, bg=BG, fg=KEY_PLAYING)
playing_label.pack(pady=(0,4))

# ── Piano canvas ──────────────────────────────────────────────────────────────
PIANO_W   = CAM_W
PIANO_H   = 110
NUM_WHITE = 14
WK_W      = PIANO_W // NUM_WHITE
WK_H      = PIANO_H - 4
BK_W      = int(WK_W * 0.58)
BK_H      = int(WK_H * 0.58)

piano_canvas = tk.Canvas(left_frame, width=PIANO_W, height=PIANO_H,
                          bg="#111111", highlightthickness=1,
                          highlightbackground=BORDER)
piano_canvas.pack()

white_key_ids = []
black_key_ids = []

def build_piano(finger_states=None, playing_finger=None):
    piano_canvas.delete("all")
    white_key_ids.clear()
    black_key_ids.clear()

    if finger_states is None:
        finger_states = [False] * 5

    notes = current_notes()

    white_finger_map = {}
    black_finger_map = {}

    for fi, note in enumerate(notes):
        is_playing = (fi == playing_finger) and finger_states[fi]
        if is_black_note(note):
            wi = BLACK_KEY_NOTE_TO_WHITE_IDX[note]
            black_finger_map[wi] = (fi, finger_states[fi], is_playing, note)
        else:
            for wi, wk_note in enumerate(WHITE_KEY_NOTES):
                if note_matches_white_key(note, wk_note):
                    white_finger_map[wi] = (fi, finger_states[fi], is_playing)
                    break

    # Draw white keys
    for i in range(NUM_WHITE):
        x0 = i * WK_W + 1
        x1 = x0 + WK_W - 2
        if i in white_finger_map:
            fi, is_down, is_playing = white_finger_map[i]
            if is_playing:
                color = KEY_PLAYING
            elif is_down:
                color = KEY_ACTIVE
            else:
                color = KEY_MAPPED
        else:
            color = KEY_WHITE
        kid = piano_canvas.create_rectangle(x0, 2, x1, WK_H,
                                             fill=color, outline="#555555", width=1)
        white_key_ids.append(kid)

    # Note labels on white finger keys
    for wi, (fi, is_down, is_playing) in white_finger_map.items():
        x_center     = wi * WK_W + WK_W // 2
        display_note = notes[fi].replace("4","").replace("5","")
        if is_playing:
            fg = "#7a4800"
        elif is_down:
            fg = GREEN_DARK
        else:
            fg = "#2a7a58"
        piano_canvas.create_text(x_center, WK_H - 10,
                                  text=display_note, font=note_fnt,
                                  fill=fg, tags="notelabel")

    # Draw black keys
    for i in range(NUM_WHITE - 1):
        if i not in BLACK_KEY_AFTER:
            black_key_ids.append(None)
            continue

        x0 = (i + 1) * WK_W - BK_W // 2

        if i in black_finger_map:
            fi, is_down, is_playing, note = black_finger_map[i]
            if is_playing:
                fill_color = KEY_PLAYING
            elif is_down:
                fill_color = "#0F6E56"         # dark green when pressed
            else:
                fill_color = "#5DCAA5"         # light green when assigned, not pressed
        else:
            fill_color = KEY_BLACK

        kid = piano_canvas.create_rectangle(x0, 2, x0 + BK_W, BK_H,
                                             fill=fill_color,
                                             outline="#000000", width=1)
        black_key_ids.append(kid)

        if i in black_finger_map:
            fi, is_down, is_playing, note = black_finger_map[i]
            display_note = (notes[fi].replace("4","").replace("5","")
                                     .replace("#","\u266f")
                                     .replace("b","\u266d"))
            x_center = x0 + BK_W // 2
            if is_playing:
                text_color = "#7a4800"
            elif is_down:
                text_color = "#E1F5EE"
            else:
                text_color = "#085041"
            piano_canvas.create_text(x_center, BK_H - 10,
                                      text=display_note, font=note_fnt,
                                      fill=text_color, tags="notelabel")

build_piano()

# ── Right panel ───────────────────────────────────────────────────────────────
tk.Label(right_frame, text="RIGHT HAND", font=small_fnt,
         bg=PANEL_BG, fg=TEXT_SEC).pack(pady=(14,6))
tk.Frame(right_frame, bg=BORDER, height=1, width=240).pack(fill="x", padx=12)

led_canvases = []
led_circles  = []
note_labels  = []

for i, name in enumerate(FINGER_NAMES):
    row = tk.Frame(right_frame, bg=PANEL_BG)
    row.pack(fill="x", padx=16, pady=5)

    tk.Label(row, text=name, font=lbl_fnt, bg=PANEL_BG,
             fg=TEXT_PRI, width=7, anchor="w").pack(side="left")

    note_lbl = tk.Label(row, text=SCALES["C major"][i],
                         font=note_fnt, bg="#0F6E56", fg=KEY_ACTIVE_TEXT,
                         padx=6, pady=2, relief="flat")
    note_lbl.pack(side="left", padx=4)
    note_labels.append(note_lbl)

    c = tk.Canvas(row, width=28, height=28, bg=PANEL_BG, highlightthickness=0)
    c.pack(side="right")
    oval = c.create_oval(4, 4, 24, 24, fill=LED_OFF, outline="")
    led_canvases.append(c)
    led_circles.append(oval)

tk.Frame(right_frame, bg=BORDER, height=1, width=240).pack(fill="x", padx=12, pady=8)

tk.Label(right_frame, text="BPM  (left hand)", font=small_fnt,
         bg=PANEL_BG, fg=TEXT_SEC).pack()
bpm_var = tk.StringVar(value="---")
tk.Label(right_frame, textvariable=bpm_var, font=large_fnt,
         bg=PANEL_BG, fg=BPM_COLOR).pack(pady=(2,10))

tk.Frame(right_frame, bg=BORDER, height=1, width=240).pack(fill="x", padx=12)

# Serial status
status_row = tk.Frame(right_frame, bg=PANEL_BG)
status_row.pack(pady=10, padx=16, fill="x")
dot_c = tk.Canvas(status_row, width=14, height=14, bg=PANEL_BG, highlightthickness=0)
dot_c.pack(side="left")
dot_c.create_oval(2, 2, 12, 12,
                   fill=GREEN if serial_connected else "#e24b4a", outline="")
tk.Label(status_row,
         text=f"{COM_PORT} connected" if serial_connected else "Serial not found",
         font=small_fnt, bg=PANEL_BG, fg=TEXT_SEC).pack(side="left", padx=6)

tk.Button(right_frame, text="Quit  [q]", font=small_fnt,
           bg=PANEL_BG, fg=TEXT_SEC, activebackground=BORDER,
           relief="flat", bd=0, cursor="hand2",
           command=root.destroy).pack(pady=(4,14))

# ── Main update loop ──────────────────────────────────────────────────────────
photo_ref = None

def update():
    global photo_ref, current_bpm

    now = time.time()

    ret, frame = cap.read()
    if not ret:
        root.after(30, update)
        return

    frame     = cv2.flip(frame, 1)
    rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    mp_image  = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)
    results   = detector.detect(mp_image)

    right_fingers = [False] * 5
    bpm           = None

    if results.hand_landmarks and results.handedness:
        for landmarks, handedness in zip(results.hand_landmarks, results.handedness):
            draw_landmarks(frame, landmarks)
            label = handedness[0].category_name

            if label == "Right":
                right_fingers[0] = is_finger_down(landmarks, 4,  3)
                right_fingers[1] = is_finger_down(landmarks, 8,  6)
                right_fingers[2] = is_finger_down(landmarks, 12, 10)
                right_fingers[3] = is_finger_down(landmarks, 16, 14)
                right_fingers[4] = is_finger_down(landmarks, 20, 18)

            if label == "Left":
                wrist_y = landmarks[0].y
                bpm     = int(200 - (wrist_y * 160))
                bpm     = max(40, min(200, bpm))
                current_bpm = bpm   # keep last known BPM in sync

    # ── Alternating note logic ─────────────────────────────────────────────────
    down_fingers = [i for i, f in enumerate(right_fingers) if f]
    update_alt_state(down_fingers)
    advance_alt_note(now, current_bpm)
    playing_fi = current_playing_finger()

    # ── Serial write (matching your original priority logic) ───────────────────
    f_str = ''.join(['1' if f else '0' for f in right_fingers])
    if ser:
        if not results.hand_landmarks:
            ser.write(b"NOHANDS\n")
        elif bpm is not None:
            ser.write(f"F:{f_str},BPM:{bpm},KEY:{current_scale_var.get()}\n".encode())
        else:
            ser.write(f"F:{f_str},BPM:{current_bpm},KEY:{current_scale_var.get()}\n".encode())

    print(f"Fingers: {f_str} | BPM: {current_bpm} | Key: {current_scale_var.get()}")

    # ── Update LEDs ────────────────────────────────────────────────────────────
    for i, state in enumerate(right_fingers):
        if state and i == playing_fi:
            led_canvases[i].itemconfig(led_circles[i], fill=KEY_PLAYING)
        elif state:
            led_canvases[i].itemconfig(led_circles[i], fill=LED_ON)
        else:
            led_canvases[i].itemconfig(led_circles[i], fill=LED_OFF)

    # ── Update piano ───────────────────────────────────────────────────────────
    build_piano(right_fingers, playing_finger=playing_fi)

    # ── Update labels ──────────────────────────────────────────────────────────
    finger_disp = "  ".join(list(f_str))
    finger_label.config(text=f"Fingers:  {finger_disp}")

    notes = current_notes()
    if playing_fi is not None:
        playing_label.config(text=f"Playing:  {notes[playing_fi]}")
    else:
        playing_label.config(text="Playing:  —")

    bpm_var.set(str(current_bpm))

    # ── Camera frame overlay ───────────────────────────────────────────────────
    active_notes = [notes[i] for i in down_fingers]
    overlay      = " ".join(active_notes) if active_notes else "---"

    cv2.putText(frame, f"Key: {current_scale_var.get()}", (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (29,158,117), 2)
    cv2.putText(frame, f"Active: {overlay}", (10, 58),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0,255,0), 2)
    cv2.putText(frame, f"Playing: {notes[playing_fi] if playing_fi is not None else '—'}",
                (10, 86), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (239,159,39), 2)
    cv2.putText(frame, f"BPM: {current_bpm}", (10, 114),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (55,138,221), 2)

    # ── Render camera frame to canvas ──────────────────────────────────────────
    display   = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    display   = cv2.resize(display, (CAM_W, CAM_H))
    photo_ref = ImageTk.PhotoImage(image=Image.fromarray(display))
    cam_canvas.create_image(0, 0, anchor="nw", image=photo_ref)

    root.after(30, update)

root.bind("<q>", lambda e: root.destroy())
root.bind("<Q>", lambda e: root.destroy())

root.after(0, update)
root.mainloop()

cap.release()
if ser:
    ser.close()
