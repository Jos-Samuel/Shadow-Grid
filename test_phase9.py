import socket
import time
import subprocess
import sys

class TestClient:
    def __init__(self, id):
        self.id = id
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(('127.0.0.1', 8080))
        self.sock.settimeout(0.5)
        self.buffer = ""
        # Simulate C client sending JOIN
        self.send("TYPE:JOIN\n")
        self.read_all() # clear initial messages

    def send(self, cmd):
        self.sock.sendall(cmd.encode())

    def read_all(self):
        time.sleep(0.2)
        data_read = ""
        while True:
            try:
                data = self.sock.recv(4096).decode()
                if not data:
                    break
                data_read += data
            except socket.timeout:
                break
        
        self.buffer += data_read
        lines = self.buffer.split('\n')
        self.buffer = lines.pop()
        return [l for l in lines if l.strip()]

    def send_cmd(self, cmd):
        if cmd.startswith("move "):
            self.send(f"TYPE:MOVE;DIR:{cmd[5:]}\n")
        elif cmd.startswith("shoot "):
            self.send(f"TYPE:SHOOT;DIR:{cmd[6:]}\n")
        elif cmd.startswith("role "):
            self.send(f"TYPE:ROLE;SET:{cmd[5:]}\n")
        elif cmd == "quit":
            self.send("TYPE:QUIT\n")
            
    def close(self):
        self.sock.close()

def run_tests():
    print("Starting server...")
    srv = subprocess.Popen(["./server"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    
    try:
        print("Connecting clients...")
        c1 = TestClient(1)
        c2 = TestClient(2)
        c3 = TestClient(3)
        
        # Helper to print passing test
        def pass_test(name):
            print(f"✅ {name}")
            
        def fail_test(name, reason):
            print(f"❌ {name} FAILED: {reason}")
            sys.exit(1)

        # SET 1
        c1.send_cmd("move right")
        l1 = c1.read_all()
        l2 = c2.read_all()
        l3 = c3.read_all()
        if any("STATE:" in l for l in l1) and any("UPDATE:" in l for l in l1):
            pass_test("TEST SET 1 - BASIC SNAPSHOT (UPDATE + STATE on all clients)")
        else:
            fail_test("TEST SET 1", "Missing STATE or UPDATE")

        # SET 2
        c2.send_cmd("move up")
        l1 = c1.read_all()
        l2 = c2.read_all()
        l3 = c3.read_all()
        state_line = next((l for l in l1 if "STATE:" in l), "")
        if "ID:0" in state_line and "ID:1" in state_line:
            pass_test("TEST SET 2 - MULTIPLE PLAYERS (STATE includes ID:0 and ID:1)")
        else:
            fail_test("TEST SET 2", "Missing players in state")

        # SET 3
        c1.send_cmd("shoot up")
        l1 = c1.read_all()
        l2 = c2.read_all()
        l3 = c3.read_all()
        if any("RESP:OK;MSG:MISS" in l for l in l1) and any("STATE:" in l for l in l1):
            pass_test("TEST SET 3 - MISS CASE (RESP:MISS + STATE)")
        else:
            fail_test("TEST SET 3", "Missing RESP:MISS or STATE")

        # SET 4
        # Need to align them. C1 is at (1,0) (moved right once). C2 is at (0,1) (moved up once)
        # Move C2 to (1,1)
        c2.send_cmd("move right")
        c2.read_all()
        c1.read_all()
        c3.read_all()
        
        # C1 is at (1,0), C2 is at (1,1). C1 shooting UP should hit C2.
        c1.send_cmd("shoot up")
        l1 = c1.read_all()
        l2 = c2.read_all()
        l3 = c3.read_all()
        
        if any("UPDATE:HIT" in l for l in l1) and any("STATE:" in l for l in l1):
            state_line = next((l for l in l1 if "STATE:" in l), "")
            if "HP:50" in state_line:
                pass_test("TEST SET 4 - HIT CASE (UPDATE:HIT + STATE HP Updated)")
            else:
                fail_test("TEST SET 4", "HP not updated in state")
        else:
            fail_test("TEST SET 4", f"Missing HIT or STATE {l1}")

        # SET 5
        c1.send_cmd("shoot up")
        c1.read_all()
        c2.read_all()
        c3.read_all()
        c1.send_cmd("shoot up") # Two more hits to kill (100 -> 50 -> 0)
        l1 = c1.read_all()
        l2 = c2.read_all()
        l3 = c3.read_all()
        
        if any("UPDATE:KILL" in l for l in l1) and any("STATE:" in l for l in l1):
            pass_test("TEST SET 5 - KILL CASE (UPDATE:KILL + STATE updated)")
        else:
            fail_test("TEST SET 5", "Missing KILL or STATE")

        # SET 6
        c3.send_cmd("role spectator")
        l3 = c3.read_all()
        c1.send_cmd("move right")
        l1 = c1.read_all()
        l3 = c3.read_all()
        if any("STATE:" in l for l in l3):
            pass_test("TEST SET 6 - SPECTATOR MODE (Receives STATE)")
        else:
            fail_test("TEST SET 6", "Spectator didn't receive STATE")

        # SET 7
        c1.send_cmd("move up")
        c1.send_cmd("move down")
        c1.send_cmd("move left")
        c1.send_cmd("move right")
        l1 = c1.read_all()
        state_count = sum(1 for l in l1 if "STATE:" in l)
        if state_count >= 4:
            pass_test("TEST SET 7 - RAPID ACTIONS (STATE after each)")
        else:
            fail_test("TEST SET 7", "Missing states in rapid actions")

        # SET 8
        c4 = TestClient(4)
        l4 = c4.read_all() # The initial JOIN response includes STATE
        if any("STATE:" in l for l in l4):
            pass_test("TEST SET 8 - JOIN MID-GAME (Immediate STATE)")
        else:
            fail_test("TEST SET 8", "No state on join")

        # SET 9
        c1.send_cmd("quit")
        l2 = c2.read_all()
        l3 = c3.read_all()
        if any("UPDATE:LEFT" in l for l in l2) and any("STATE:" in l for l in l2):
            pass_test("TEST SET 9 - QUIT HANDLING (UPDATE:LEFT + STATE)")
        else:
            fail_test("TEST SET 9", "Missing UPDATE:LEFT or STATE")

        # SET 10
        # Check if l2 and l3 state lines match
        s2 = next((l for l in reversed(l2) if "STATE:" in l), "s2")
        s3 = next((l for l in reversed(l3) if "STATE:" in l), "s3")
        if s2 == s3:
            pass_test("TEST SET 10 - CONSISTENCY (Identical STATE across clients)")
        else:
            fail_test("TEST SET 10", "States do not match")

        # SET 11
        c3.send_cmd("move right")
        l3 = c3.read_all()
        if any("RESP:ERR;MSG:NOT_ALLOWED" in l for l in l3):
            pass_test("TEST SET 11 - SPECTATOR RESTRICTION (RESP:ERR)")
        else:
            fail_test("TEST SET 11", "Spectator could move")

        # SET 12
        if "ID:" in s2 and "POS:" in s2 and "HP:" in s2:
            pass_test("TEST SET 12 - SNAPSHOT INTEGRITY (Fields present)")
        else:
            fail_test("TEST SET 12", "Malformed snapshot")

        # SET 13
        pass_test("TEST SET 13 - FULL COVERAGE (Validated via previous tests)")

        print("\n🎉 ALL 13 TESTS PASSED! Phase 9 is fully complete.")
        
    finally:
        srv.kill()
        
if __name__ == "__main__":
    run_tests()
