import socket
import threading
import time
import sys
import random

HOST = '127.0.0.1'
PORT = 8080
MAX_TEST_CLIENTS = 105
SIMULATION_TIME = 10 # seconds

active_sockets = []
connection_results = []
lock = threading.Lock()
running = True

def socket_reader(client_id, is_admin, s):
    s.settimeout(1.0)
    while running:
        try:
            data = s.recv(65536).decode('utf-8', errors='ignore')
            if not data:
                break
                
            # Scan the broadcast/response stream for rich game events
            if "DEADLOCK_BROKEN" in data:
                with lock:
                    print(f"[DEADLOCK] Client {client_id:03d}'s invite triggered a Wait-For Graph cycle! Server aborted it.")
            elif "FRIENDLY_FIRE_BLOCKED" in data:
                with lock:
                    print(f"[FRIENDLY FIRE] Client {client_id:03d} shot a squadmate. Blocked by server!")
            elif "HIT_BLOCKED" in data:
                with lock:
                    print(f"[SHIELD] Client {client_id:03d} shot an Admin. Shot was harmlessly blocked!")
            elif "HIT_KILL" in data:
                with lock:
                    print(f"[KILL] Client {client_id:03d} landed a lethal shot and scored a point!")
            elif "HIT;" in data:
                with lock:
                    print(f"[HIT] Client {client_id:03d} successfully shot an active player!")
            elif "RESP:OK;MSG:INVITED" in data:
                with lock:
                    print(f"[ALLIANCE] Admin Client {client_id:03d} successfully invited a player to their squad.")
            elif "YOU_DIED" in data:
                with lock:
                    print(f"[DEATH] Client {client_id:03d} was killed! Respawning...")
                # Instantly respawn so we can get back in the fight!
                s.sendall(b"TYPE:RESPAWN\n")
                    
        except socket.timeout:
            continue
        except Exception:
            break

def client_behavior(client_id, is_admin, s):
    # Join the grid first
    try:
        s.sendall(b"TYPE:JOIN\n")
        time.sleep(0.1)
        
        # If even ID, elevate to Admin so we can test invites and deadlocks
        if is_admin:
            s.sendall(b"TYPE:ROLE;SET:admin;PASS:secret123\n")
            time.sleep(0.1)
    except:
        return

    # Start the continuous reader thread
    reader_thread = threading.Thread(target=socket_reader, args=(client_id, is_admin, s))
    reader_thread.daemon = True
    reader_thread.start()

    move_actions = ["TYPE:MOVE;DIR:UP\n", "TYPE:MOVE;DIR:DOWN\n", "TYPE:MOVE;DIR:LEFT\n", "TYPE:MOVE;DIR:RIGHT\n"]
    shoot_actions = ["TYPE:SHOOT;DIR:UP\n", "TYPE:SHOOT;DIR:DOWN\n", "TYPE:SHOOT;DIR:LEFT\n", "TYPE:SHOOT;DIR:RIGHT\n"]
    
    end_time = time.time() + SIMULATION_TIME
    while running and time.time() < end_time:
        try:
            # Admins (Even IDs) focus on squad mechanics & invites
            # Players (Odd IDs) focus on movement & shooting
            if is_admin:
                roll = random.random()
                if roll < 0.30:
                    target = random.randint(1, MAX_TEST_CLIENTS)
                    s.sendall(f"TYPE:INVITE;TARGET:{target}\n".encode('utf-8'))
                elif roll < 0.40:
                    t1 = random.randint(1, MAX_TEST_CLIENTS)
                    t2 = random.randint(1, MAX_TEST_CLIENTS)
                    s.sendall(f"TYPE:CREATE_SQUAD;TARGETS:{t1},{t2}\n".encode('utf-8'))
                else:
                    cmd = random.choice(move_actions)
                    s.sendall(cmd.encode('utf-8'))
            else:
                roll = random.random()
                if roll < 0.40:
                    cmd = random.choice(move_actions)
                else:
                    cmd = random.choice(shoot_actions)
                s.sendall(cmd.encode('utf-8'))
                
            time.sleep(random.uniform(0.02, 0.08)) # Fast actions
        except Exception:
            break

def connect_client(client_id):
    is_admin = (client_id % 2 == 0)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect((HOST, PORT))
        
        s.settimeout(2.0)
        data = s.recv(1024).decode('utf-8')
        
        if "CONNECTED" in data:
            with lock:
                active_sockets.append(s)
                role_name = "Admin" if is_admin else "Player"
                print(f"[SUCCESS] Client {client_id:03d} ({role_name}) connected.")
                connection_results.append((client_id, True))
                
            client_behavior(client_id, is_admin, s)
            
    except socket.timeout:
        with lock:
            print(f"[BLOCKED] Client {client_id:03d} timed out! Semaphore rejected connection.")
            connection_results.append((client_id, False))
    except Exception as e:
        with lock:
            print(f"[ERROR] Client {client_id:03d} error: {e}")
            connection_results.append((client_id, False))

if __name__ == "__main__":
    print(f"=== SHADOW-GRID RACE CONDITION & SEMAPHORE TEST ===")
    print(f"Spawning {MAX_TEST_CLIENTS} threads to hammer the server for {SIMULATION_TIME} seconds...")
    print(f"Admins (Even IDs) will manage alliances; Players (Odd IDs) will fight on the grid.")
    print(f"--------------------------------------------------\n")
    
    threads = []
    
    for i in range(1, MAX_TEST_CLIENTS + 1):
        t = threading.Thread(target=connect_client, args=(i,))
        threads.append(t)
        t.start()
        time.sleep(0.005)
        
    for t in threads:
        t.join()
        
    running = False
    
    print(f"\n--------------------------------------------------")
    print(f"=== STRESS TEST COMPLETE ===")
    successful_connections = sum(1 for _, success in connection_results if success)
    blocked_connections = len(connection_results) - successful_connections
    
    print(f"Total Connections Accepted: {successful_connections}")
    print(f"Total Connections Blocked:  {blocked_connections}")
    
    for s in active_sockets:
        try:
            s.sendall(b"TYPE:QUIT\n")
            s.close()
        except:
            pass
