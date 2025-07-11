import pygame
import random
import math
import socket
import json
import time
import threading
import queue

HOST = '127.0.0.1'
PORT = 55555

client_socket = None
send_queue = queue.Queue()

def connect():
    global client_socket
    try:
        client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client_socket.connect((HOST, PORT))

        # Send CONNECT message immediately, bypassing queue
        connect_message = {"type": "CONNECT"}
        client_socket.send(json.dumps(connect_message).encode() + b"\n")

        # Wait for response (blocking)
        data = client_socket.recv(1024)
        try:
            response = json.loads(data.decode())
            if response.get("type") == "response" and response.get("status") == "SUCCESS":
                print("Connected to Server!")
                client_socket.setblocking(False)  # switch to non-blocking after handshake
                return True
            else:
                print("Unexpected server response:", response)
        except Exception as e:
            print("Failed to parse response:", e)

    except Exception as e:
        print(f"Connection error: {e}")

    return False

def sender_thread():
    global client_socket
    while True:
        try:
            msg = send_queue.get()
            if client_socket:
                client_socket.send(msg)
        except BlockingIOError:
            # Send buffer is full; requeue or skip depending on use case
            time.sleep(0.01)
        except Exception as e:
            print(f"Sender thread error: {e}")
            break

def send_message(msg: dict):
    send_queue.put(json.dumps(msg).encode() + b"\n")

def disconnect():
    global client_socket
    if client_socket:
        client_socket.close()
        client_socket = None
        print("Disconnected from server.")

# Receive authoritative updates in background
authoritative_state = None
def receive_thread():
    global authoritative_state
    buffer = ""
    while True:
        try:
            data = client_socket.recv(1024)
            if not data:
                break
            buffer += data.decode()
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                try:
                    msg = json.loads(line)
                    if msg.get("type") == "authoritative":
                        authoritative_state = msg.get("state")
                        print("Received authoritative state:", authoritative_state)
                except Exception as e:
                    print("Bad message from server:", e)
        except BlockingIOError:
            time.sleep(0.01)
        except Exception as e:
            print("Receiver error:", e)
            break

# --------------------- MAIN GAME -------------------------
def run_game():
    global authoritative_state

    pygame.init()
    WIDTH, HEIGHT = 800, 600
    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("Networked Bouncing Square")

    SQUARE_SIZE = 80
    x = random.randint(0, WIDTH - SQUARE_SIZE)
    y = random.randint(0, HEIGHT - SQUARE_SIZE)
    velocity = 3 * math.sqrt(2)
    direction = 5
    color = (255, 0, 0)

    clock = pygame.time.Clock()
    last_sent_state = {}
    last_snapshot_time = time.time()

    # Start networking threads
    threading.Thread(target=receive_thread, daemon=True).start()
    threading.Thread(target=sender_thread, daemon=True).start()

    running = True
    while running:
        screen.fill((0, 0, 0))
        pygame.draw.rect(screen, color, (x, y, SQUARE_SIZE, SQUARE_SIZE))
        pygame.display.flip()

        rad = math.radians(direction)
        dx = velocity * math.cos(rad)
        dy = velocity * math.sin(rad)
        x += dx
        y += dy

        if x <= 0 or x + SQUARE_SIZE >= WIDTH:
            direction = 180 - direction
        if y <= 0 or y + SQUARE_SIZE >= HEIGHT:
            direction = -direction
        direction %= 360

        # Apply authoritative state if received
        if authoritative_state:
            x = authoritative_state.get("x", x)
            y = authoritative_state.get("y", y)
            direction = authoritative_state.get("direction", direction)
            color = tuple(authoritative_state.get("color", color))
            velocity = authoritative_state.get("velocity", velocity)
            authoritative_state = None

        current_state = {
            "x": round(x, 2),
            "y": round(y, 2),
            "velocity": round(velocity, 2),
            "direction": round(direction, 2),
            "color": color
        }

        # Send delta update
        delta = {k: v for k, v in current_state.items() if last_sent_state.get(k) != v}
        if delta:
            send_message({"type": "delta", "state": delta})
            last_sent_state.update(delta)

        # Periodic snapshot
        now = time.time()
        if now - last_snapshot_time >= 2.0:
            send_message({"type": "snapshot", "state": current_state})
            last_snapshot_time = now

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        clock.tick(60)

    pygame.quit()
    disconnect()

def main():
    if connect():
        run_game()
    else:
        print("Failed to connect.")
        return 1
    return 0

if __name__ == "__main__":
    main()
