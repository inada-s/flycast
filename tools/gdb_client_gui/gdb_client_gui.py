#!/usr/bin/env python3
"""
SH4/Dreamcast GDB Client GUI for Flycast emulator
Based on sh4-dc.gdb configuration
"""

import subprocess
import threading
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, simpledialog
import queue
import re
import time
import json
import os
import ast
import operator


# Profile directory
PROFILES_DIR = "profiles"


class ProfileManager:
    """Manages profiles for saving breakpoints and watches per game"""

    CONFIG_FILE = "gdb_client_config.json"

    def __init__(self):
        self.current_profile = "default"
        self._load_config()
        self._ensure_profile_dir()

    def _load_config(self):
        """Load config file"""
        if os.path.exists(self.CONFIG_FILE):
            try:
                with open(self.CONFIG_FILE, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    self.current_profile = data.get("current_profile", "default")
            except:
                pass

    def _save_config(self):
        """Save config file"""
        try:
            with open(self.CONFIG_FILE, 'w', encoding='utf-8') as f:
                json.dump({"current_profile": self.current_profile}, f, indent=2)
        except:
            pass

    def _ensure_profile_dir(self):
        """Ensure profile directory exists"""
        profile_path = self.get_profile_path()
        os.makedirs(profile_path, exist_ok=True)

    def get_profile_path(self, profile_name=None):
        """Get path for a profile"""
        name = profile_name or self.current_profile
        return os.path.join(PROFILES_DIR, name)

    def get_breakpoints_file(self, profile_name=None):
        """Get breakpoints file path for profile"""
        return os.path.join(self.get_profile_path(profile_name), "breakpoints.json")

    def get_watches_file(self, profile_name=None):
        """Get watches file path for profile"""
        return os.path.join(self.get_profile_path(profile_name), "memory_watches.json")

    def list_profiles(self):
        """List all profiles"""
        profiles = []
        if os.path.exists(PROFILES_DIR):
            for name in os.listdir(PROFILES_DIR):
                if os.path.isdir(os.path.join(PROFILES_DIR, name)):
                    profiles.append(name)
        if not profiles:
            profiles = ["default"]
        return sorted(profiles)

    def switch_profile(self, profile_name):
        """Switch to a different profile"""
        self.current_profile = profile_name
        self._ensure_profile_dir()
        self._save_config()

    def create_profile(self, profile_name):
        """Create a new profile"""
        profile_path = self.get_profile_path(profile_name)
        os.makedirs(profile_path, exist_ok=True)
        return profile_name

    def delete_profile(self, profile_name):
        """Delete a profile"""
        if profile_name == "default":
            return False
        profile_path = self.get_profile_path(profile_name)
        if os.path.exists(profile_path):
            import shutil
            shutil.rmtree(profile_path)
            if self.current_profile == profile_name:
                self.switch_profile("default")
            return True
        return False

    def rename_profile(self, old_name, new_name):
        """Rename a profile"""
        if old_name == "default":
            return False
        old_path = self.get_profile_path(old_name)
        new_path = self.get_profile_path(new_name)
        if os.path.exists(old_path) and not os.path.exists(new_path):
            os.rename(old_path, new_path)
            if self.current_profile == old_name:
                self.current_profile = new_name
                self._save_config()
            return True
        return False


# Dreamcast Memory Map Constants
DC_RAM_P0 = 0x0C000000
DC_RAM_P1 = 0x8C000000
DC_RAM_P2 = 0xAC000000
DC_RAM_SIZE = 0x01000000
DC_VRAM = 0xA5000000
DC_VRAM_SIZE = 0x00800000
DC_BOOTROM = 0xA0000000
DC_FLASH = 0x00200000
DC_PVR_BASE = 0xA05F8000
DC_AICA_BASE = 0x00700000
DC_GDROM_BASE = 0x005F7000


class GDBClient:
    def __init__(self, on_status_change=None):
        self.process = None
        self.output_queue = queue.Queue()
        self.reader_thread = None
        self.running = False
        self.connected = False
        self.target = "127.0.0.1:3263"
        self.on_status_change = on_status_change
        self.last_response_time = time.time()

    def start(self, target="127.0.0.1:3263"):
        """Start GDB and connect to target"""
        self.stop()
        self.target = target
        self.running = True
        self.last_response_time = time.time()

        # Start GDB with SH4 architecture
        self.process = subprocess.Popen(
            ["gdb-multiarch", "--quiet", "--interpreter=mi"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1
        )

        # Start output reader thread
        self.reader_thread = threading.Thread(target=self._read_output, daemon=True)
        self.reader_thread.start()

        # Initialize SH4 settings
        self._send_raw("-gdb-set architecture sh4")
        self._send_raw("-gdb-set endian little")
        # Enable async mode for interrupt support
        self._send_raw("-gdb-set mi-async on")
        self._send_raw("-gdb-set target-async on")
        self._send_raw(f"-target-select remote {self.target}")
        self.connected = True

    def stop(self):
        """Stop GDB process"""
        self.running = False
        self.connected = False
        if self.process:
            try:
                self._send_raw("-gdb-exit")
                self.process.wait(timeout=2)
            except:
                pass
            try:
                self.process.kill()
            except:
                pass
            self.process = None

    def notify_response(self):
        """Call this when a response is received from GDB"""
        self.last_response_time = time.time()

    def get_idle_time(self):
        """Get seconds since last response"""
        return time.time() - self.last_response_time

    def _send_raw(self, cmd):
        """Send raw MI command"""
        if self.process and self.process.stdin:
            try:
                self.process.stdin.write(cmd + "\n")
                self.process.stdin.flush()
            except:
                pass

    def send_command(self, cmd):
        """Send CLI command via MI interface"""
        self._send_raw(f"-interpreter-exec console \"{cmd}\"")

    def stepi(self):
        self._send_raw("-exec-step-instruction")

    def nexti(self):
        self._send_raw("-exec-next-instruction")

    def continue_exec(self):
        self._send_raw("-exec-continue")

    def interrupt(self):
        """Send interrupt to stop execution"""
        self._send_raw("-exec-interrupt --all")

    def get_registers(self):
        self._send_raw("-data-list-register-values x")

    def disassemble(self, addr, count=20):
        self._send_raw(f"-data-disassemble -s {addr} -e {addr + count * 2} -- 0")

    def read_memory(self, addr, length):
        self._send_raw(f"-data-read-memory {addr} x 4 1 {length // 4}")

    def set_breakpoint(self, addr):
        self._send_raw(f"-break-insert *{addr}")

    def set_watchpoint(self, addr, wp_type="write"):
        """Set a watchpoint at address
        wp_type: 'write', 'read', or 'access'
        """
        # GDB/MI: -break-watch [-a | -r] <expr>
        # -a: access (read/write), -r: read, none: write
        if wp_type == "read":
            self._send_raw(f"-break-watch -r *{addr}")
        elif wp_type == "access":
            self._send_raw(f"-break-watch -a *{addr}")
        else:  # write
            self._send_raw(f"-break-watch *{addr}")

    def delete_breakpoint(self, num):
        self._send_raw(f"-break-delete {num}")

    def list_breakpoints(self):
        self._send_raw("-break-list")

    def _read_output(self):
        """Read GDB output in background thread"""
        while self.running and self.process:
            try:
                line = self.process.stdout.readline()
                if line:
                    self.output_queue.put(line)
                elif self.process.poll() is not None:
                    self.output_queue.put("[GDB process terminated]\n")
                    break
            except:
                break


class RegisterPanel(ttk.LabelFrame):
    """Panel to display SH4 registers"""

    # SH4 register names in order from GDB
    REG_NAMES = [
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "pc", "pr", "gbr", "vbr", "mach", "macl", "sr",
        "fpul", "fpscr",
        "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7",
        "fr8", "fr9", "fr10", "fr11", "fr12", "fr13", "fr14", "fr15",
        "ssr", "spc",
        "r0b0", "r1b0", "r2b0", "r3b0", "r4b0", "r5b0", "r6b0", "r7b0",
        "r0b1", "r1b1", "r2b1", "r3b1", "r4b1", "r5b1", "r6b1", "r7b1",
    ]

    def __init__(self, parent):
        super().__init__(parent, text="SH4 Registers")
        self.reg_labels = {}
        self.reg_values = {}
        self._create_widgets()

    def _create_widgets(self):
        # General registers r0-r15
        gen_frame = ttk.LabelFrame(self, text="General (R0-R15)")
        gen_frame.pack(fill=tk.X, padx=2, pady=2)

        for i in range(16):
            row, col = divmod(i, 4)
            name = f"r{i}"
            self.reg_values[name] = tk.StringVar(value="--------")
            frame = ttk.Frame(gen_frame)
            frame.grid(row=row, column=col, padx=2, pady=1, sticky="w")
            ttk.Label(frame, text=f"R{i:02d}:", width=4).pack(side=tk.LEFT)
            lbl = ttk.Label(frame, textvariable=self.reg_values[name],
                           font=("Courier", 9), foreground="blue")
            lbl.pack(side=tk.LEFT)
            self.reg_labels[name] = lbl

        # System registers
        sys_frame = ttk.LabelFrame(self, text="System")
        sys_frame.pack(fill=tk.X, padx=2, pady=2)

        sys_regs = [("pc", "PC"), ("pr", "PR"), ("sr", "SR"),
                    ("gbr", "GBR"), ("vbr", "VBR"),
                    ("mach", "MACH"), ("macl", "MACL")]

        for i, (name, label) in enumerate(sys_regs):
            row, col = divmod(i, 4)
            self.reg_values[name] = tk.StringVar(value="--------")
            frame = ttk.Frame(sys_frame)
            frame.grid(row=row, column=col, padx=2, pady=1, sticky="w")
            ttk.Label(frame, text=f"{label}:", width=5).pack(side=tk.LEFT)
            lbl = ttk.Label(frame, textvariable=self.reg_values[name],
                           font=("Courier", 9), foreground="darkgreen")
            lbl.pack(side=tk.LEFT)
            self.reg_labels[name] = lbl

        # FPU registers (collapsed by default)
        self.fpu_frame = ttk.LabelFrame(self, text="FPU (FR0-FR15)")

        for i in range(16):
            row, col = divmod(i, 4)
            name = f"fr{i}"
            self.reg_values[name] = tk.StringVar(value="--------")
            frame = ttk.Frame(self.fpu_frame)
            frame.grid(row=row, column=col, padx=2, pady=1, sticky="w")
            ttk.Label(frame, text=f"FR{i:02d}:", width=5).pack(side=tk.LEFT)
            lbl = ttk.Label(frame, textvariable=self.reg_values[name],
                           font=("Courier", 9), foreground="purple")
            lbl.pack(side=tk.LEFT)
            self.reg_labels[name] = lbl

        # FPU control
        fpu_ctrl = ttk.Frame(self.fpu_frame)
        fpu_ctrl.grid(row=4, column=0, columnspan=4, pady=2)
        for name in ["fpul", "fpscr"]:
            self.reg_values[name] = tk.StringVar(value="--------")
            ttk.Label(fpu_ctrl, text=f"{name.upper()}:").pack(side=tk.LEFT, padx=2)
            ttk.Label(fpu_ctrl, textvariable=self.reg_values[name],
                     font=("Courier", 9)).pack(side=tk.LEFT, padx=5)

        # Toggle FPU visibility
        self.show_fpu = tk.BooleanVar(value=False)
        ttk.Checkbutton(self, text="Show FPU Registers",
                       variable=self.show_fpu,
                       command=self._toggle_fpu).pack(anchor="w", padx=2)

    def _toggle_fpu(self):
        if self.show_fpu.get():
            self.fpu_frame.pack(fill=tk.X, padx=2, pady=2)
        else:
            self.fpu_frame.pack_forget()

    def update_registers(self, reg_data):
        """Update register display from parsed data"""
        for name, value in reg_data.items():
            if name in self.reg_values:
                old_val = self.reg_values[name].get()
                new_val = value if value else "--------"
                self.reg_values[name].set(new_val)

                # Highlight changed registers
                if name in self.reg_labels:
                    if old_val != new_val and old_val != "--------":
                        self.reg_labels[name].configure(foreground="red")
                    else:
                        # Reset color based on register type
                        if name.startswith("r") and name[1:].isdigit():
                            self.reg_labels[name].configure(foreground="blue")
                        elif name.startswith("fr"):
                            self.reg_labels[name].configure(foreground="purple")
                        else:
                            self.reg_labels[name].configure(foreground="darkgreen")


class DisassemblyPanel(ttk.LabelFrame):
    """Panel to display disassembly as a list"""

    def __init__(self, parent, on_add_breakpoint=None):
        super().__init__(parent, text="Disassembly")
        self.on_add_breakpoint = on_add_breakpoint
        self.current_pc = None
        self.lines_data = []  # Store (addr, opcode, instr) tuples
        self._create_widgets()

    def _create_widgets(self):
        # Treeview for disassembly
        columns = ("address", "instruction")
        self.tree = ttk.Treeview(self, columns=columns, show="headings", height=20)

        self.tree.heading("address", text="Address")
        self.tree.heading("instruction", text="Instruction")

        self.tree.column("address", width=100, anchor="w")
        self.tree.column("instruction", width=300, anchor="w")

        # Scrollbar
        scrollbar = ttk.Scrollbar(self, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)

        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=2, pady=2)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y, pady=2)

        # Configure tags for highlighting
        self.tree.tag_configure("current_pc", background="yellow")
        self.tree.tag_configure("breakpoint", background="#ffcccc")

        # Bind events
        self.tree.bind("<Double-1>", self._on_double_click)
        self.tree.bind("<Button-3>", self._on_right_click)  # Right click

        # Context menu
        self.context_menu = tk.Menu(self, tearoff=0)
        self.context_menu.add_command(label="Copy Address", command=self._copy_address)
        self.context_menu.add_command(label="Copy Instruction", command=self._copy_instruction)
        self.context_menu.add_separator()
        self.context_menu.add_command(label="Add Breakpoint", command=self._add_breakpoint)
        self.context_menu.add_command(label="Add to Memory Watch", command=self._add_to_watch)

    def update_disassembly(self, lines, current_pc=None):
        """Update disassembly display"""
        self.current_pc = current_pc
        self.lines_data = lines

        # Clear existing items
        self.tree.delete(*self.tree.get_children())

        # Add new items
        for addr, opcode, instr in lines:
            addr_str = f"0x{addr:08X}"
            # Replace tabs with spaces (both actual tabs and escaped \t)
            instr = instr.replace("\\t", " ").replace("\t", " ")
            # Determine tags
            tags = []
            if current_pc and addr == current_pc:
                tags.append("current_pc")

            item_id = self.tree.insert("", tk.END, iid=str(addr),
                                       values=(addr_str, instr), tags=tags)

        # Scroll to current PC
        if current_pc:
            try:
                self.tree.see(str(current_pc))
                self.tree.selection_set(str(current_pc))
            except:
                pass

    def _get_selected_address(self):
        """Get the address of the selected item"""
        selection = self.tree.selection()
        if selection:
            try:
                return int(selection[0])
            except:
                pass
        return None

    def _on_double_click(self, event):
        """Handle double-click - add breakpoint"""
        self._add_breakpoint()

    def _on_right_click(self, event):
        """Handle right-click - show context menu"""
        # Select the item under cursor
        item = self.tree.identify_row(event.y)
        if item:
            self.tree.selection_set(item)
            self.context_menu.post(event.x_root, event.y_root)

    def _copy_address(self):
        """Copy selected address to clipboard"""
        addr = self._get_selected_address()
        if addr:
            addr_str = f"0x{addr:08X}"
            self.clipboard_clear()
            self.clipboard_append(addr_str)

    def _copy_instruction(self):
        """Copy selected instruction to clipboard"""
        selection = self.tree.selection()
        if selection:
            values = self.tree.item(selection[0], "values")
            if values and len(values) >= 2:
                self.clipboard_clear()
                self.clipboard_append(values[1])

    def _add_breakpoint(self):
        """Add breakpoint at selected address"""
        addr = self._get_selected_address()
        if addr and self.on_add_breakpoint:
            self.on_add_breakpoint(addr)

    def _add_to_watch(self):
        """Add selected address to memory watch (placeholder)"""
        addr = self._get_selected_address()
        if addr:
            addr_str = f"0x{addr:08X}"
            self.clipboard_clear()
            self.clipboard_append(addr_str)


class MemoryPanel(ttk.LabelFrame):
    """Panel to display memory dump"""

    def __init__(self, parent):
        super().__init__(parent, text="Memory")
        self._create_widgets()

    def _create_widgets(self):
        # Address input
        addr_frame = ttk.Frame(self)
        addr_frame.pack(fill=tk.X, padx=2, pady=2)

        ttk.Label(addr_frame, text="Address:").pack(side=tk.LEFT)
        self.addr_entry = ttk.Entry(addr_frame, width=12, font=("Courier", 9))
        self.addr_entry.pack(side=tk.LEFT, padx=2)
        self.addr_entry.insert(0, "0x8C000000")

        # Quick access buttons
        ttk.Button(addr_frame, text="RAM", width=5,
                  command=lambda: self._set_addr(DC_RAM_P1)).pack(side=tk.LEFT, padx=1)
        ttk.Button(addr_frame, text="VRAM", width=5,
                  command=lambda: self._set_addr(DC_VRAM)).pack(side=tk.LEFT, padx=1)
        ttk.Button(addr_frame, text="ROM", width=5,
                  command=lambda: self._set_addr(DC_BOOTROM)).pack(side=tk.LEFT, padx=1)

        self.dump_btn = ttk.Button(addr_frame, text="Dump", width=6)
        self.dump_btn.pack(side=tk.LEFT, padx=5)

        # Memory display
        self.text = scrolledtext.ScrolledText(
            self, wrap=tk.NONE, font=("Courier", 9),
            height=10, width=70
        )
        self.text.pack(fill=tk.BOTH, expand=True, padx=2, pady=2)

    def _set_addr(self, addr):
        self.addr_entry.delete(0, tk.END)
        self.addr_entry.insert(0, f"0x{addr:08X}")

    def get_address(self):
        try:
            return int(self.addr_entry.get(), 0)
        except:
            return DC_RAM_P1

    def update_memory(self, addr, data):
        """Update memory display"""
        self.text.delete("1.0", tk.END)
        for i in range(0, len(data), 4):
            line_addr = addr + i * 4
            words = data[i:i+4]
            hex_str = " ".join(f"{w:08X}" for w in words)
            # ASCII representation
            ascii_str = ""
            for w in words:
                for b in w.to_bytes(4, 'little'):
                    ascii_str += chr(b) if 32 <= b < 127 else "."
            self.text.insert(tk.END, f"0x{line_addr:08X}:  {hex_str}  |{ascii_str}|\n")


class SafeExprEvaluator:
    """Safe expression evaluator for address calculations"""

    # Allowed operators
    OPERATORS = {
        ast.Add: operator.add,
        ast.Sub: operator.sub,
        ast.Mult: operator.mul,
        ast.Div: operator.floordiv,
        ast.BitOr: operator.or_,
        ast.BitAnd: operator.and_,
        ast.BitXor: operator.xor,
        ast.LShift: operator.lshift,
        ast.RShift: operator.rshift,
        ast.USub: operator.neg,
    }

    # Predefined constants
    CONSTANTS = {
        "DC_RAM_P0": DC_RAM_P0,
        "DC_RAM_P1": DC_RAM_P1,
        "DC_RAM_P2": DC_RAM_P2,
        "DC_RAM": DC_RAM_P1,
        "DC_VRAM": DC_VRAM,
        "DC_BOOTROM": DC_BOOTROM,
        "DC_FLASH": DC_FLASH,
        "DC_PVR_BASE": DC_PVR_BASE,
        "DC_AICA_BASE": DC_AICA_BASE,
        "DC_GDROM_BASE": DC_GDROM_BASE,
    }

    @classmethod
    def evaluate(cls, expr):
        """Safely evaluate an expression string to an integer address"""
        try:
            # Replace constants in expression
            for name, value in cls.CONSTANTS.items():
                expr = re.sub(rf'\b{name}\b', str(value), expr)

            # If expression looks like a hex number without 0x prefix, add it
            # Matches: 0c123456, 8c010000, etc. (starts with hex digit, all hex chars)
            if re.match(r'^[0-9a-fA-F]+$', expr.strip()):
                expr = '0x' + expr.strip()

            tree = ast.parse(expr, mode='eval')
            return cls._eval_node(tree.body)
        except Exception as e:
            raise ValueError(f"Invalid expression: {e}")

    @classmethod
    def _eval_node(cls, node):
        if isinstance(node, ast.Constant):  # Python 3.8+
            if isinstance(node.value, (int, float)):
                return int(node.value)
            raise ValueError(f"Unsupported constant type: {type(node.value)}")
        elif isinstance(node, ast.BinOp):
            left = cls._eval_node(node.left)
            right = cls._eval_node(node.right)
            op = cls.OPERATORS.get(type(node.op))
            if op:
                return op(left, right)
            raise ValueError(f"Unsupported operator: {type(node.op)}")
        elif isinstance(node, ast.UnaryOp):
            operand = cls._eval_node(node.operand)
            op = cls.OPERATORS.get(type(node.op))
            if op:
                return op(operand)
            raise ValueError(f"Unsupported unary operator: {type(node.op)}")
        else:
            raise ValueError(f"Unsupported expression type: {type(node)}")


class BreakpointManager:
    """Manages breakpoints with file persistence"""

    # Breakpoint types
    TYPE_EXECUTION = "execution"  # Normal breakpoint
    TYPE_WRITE = "write"          # Write watchpoint (break on write)
    TYPE_READ = "read"            # Read watchpoint (break on read)
    TYPE_ACCESS = "access"        # Access watchpoint (break on read or write)

    TYPES = [TYPE_EXECUTION, TYPE_WRITE, TYPE_READ, TYPE_ACCESS]
    TYPE_LABELS = {
        TYPE_EXECUTION: "Exec",
        TYPE_WRITE: "Write",
        TYPE_READ: "Read",
        TYPE_ACCESS: "R/W",
    }

    def __init__(self, filepath=None):
        self.filepath = filepath or "breakpoints.json"
        self.breakpoints = []  # List of breakpoint dicts
        self.next_id = 1
        self.load()

    def set_filepath(self, filepath):
        """Change filepath and reload"""
        self.filepath = filepath
        self.load()

    def load(self):
        """Load breakpoints from file"""
        if os.path.exists(self.filepath):
            try:
                with open(self.filepath, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    self.breakpoints = data.get("breakpoints", [])
                    self.next_id = data.get("next_id", 1)
            except Exception as e:
                print(f"Failed to load breakpoints: {e}")
                self.breakpoints = []

    def save(self):
        """Save breakpoints to file"""
        try:
            data = {
                "breakpoints": self.breakpoints,
                "next_id": self.next_id
            }
            with open(self.filepath, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
        except Exception as e:
            print(f"Failed to save breakpoints: {e}")

    def add(self, expression, comment="", bp_type=None):
        """Add a new breakpoint"""
        if bp_type is None:
            bp_type = self.TYPE_EXECUTION
        address = SafeExprEvaluator.evaluate(expression)
        bp = {
            "id": self.next_id,
            "expression": expression,
            "address": address,
            "enabled": True,
            "comment": comment,
            "type": bp_type,  # execution, write, read, access
            "gdb_num": None  # GDB's breakpoint number
        }
        self.next_id += 1
        self.breakpoints.append(bp)
        self.save()
        return bp

    def remove(self, bp_id):
        """Remove a breakpoint by ID"""
        self.breakpoints = [bp for bp in self.breakpoints if bp["id"] != bp_id]
        self.save()

    def toggle(self, bp_id):
        """Toggle breakpoint enabled state"""
        for bp in self.breakpoints:
            if bp["id"] == bp_id:
                bp["enabled"] = not bp["enabled"]
                self.save()
                return bp
        return None

    def set_enabled(self, bp_id, enabled):
        """Set breakpoint enabled state"""
        for bp in self.breakpoints:
            if bp["id"] == bp_id:
                bp["enabled"] = enabled
                self.save()
                return bp
        return None

    def update_comment(self, bp_id, comment):
        """Update breakpoint comment"""
        for bp in self.breakpoints:
            if bp["id"] == bp_id:
                bp["comment"] = comment
                self.save()
                return bp
        return None

    def update_expression(self, bp_id, expression):
        """Update breakpoint expression"""
        address = SafeExprEvaluator.evaluate(expression)
        for bp in self.breakpoints:
            if bp["id"] == bp_id:
                bp["expression"] = expression
                bp["address"] = address
                self.save()
                return bp
        return None

    def update_type(self, bp_id, bp_type):
        """Update breakpoint type"""
        for bp in self.breakpoints:
            if bp["id"] == bp_id:
                bp["type"] = bp_type
                self.save()
                return bp
        return None

    def set_gdb_num(self, bp_id, gdb_num):
        """Set GDB's breakpoint number"""
        for bp in self.breakpoints:
            if bp["id"] == bp_id:
                bp["gdb_num"] = gdb_num
                return bp
        return None

    def clear_gdb_nums(self):
        """Clear all GDB breakpoint numbers (on disconnect)"""
        for bp in self.breakpoints:
            bp["gdb_num"] = None

    def get_enabled(self):
        """Get all enabled breakpoints"""
        return [bp for bp in self.breakpoints if bp["enabled"]]

    def get_by_id(self, bp_id):
        """Get breakpoint by ID"""
        for bp in self.breakpoints:
            if bp["id"] == bp_id:
                return bp
        return None

    def get_by_gdb_num(self, gdb_num):
        """Get breakpoint by GDB number"""
        for bp in self.breakpoints:
            if bp["gdb_num"] == gdb_num:
                return bp
        return None


class BreakpointPanel(ttk.LabelFrame):
    """Panel to manage breakpoints with file persistence"""

    def __init__(self, parent, on_sync_request=None, filepath=None):
        super().__init__(parent, text="Breakpoints")
        self.manager = BreakpointManager(filepath=filepath)
        self.on_sync_request = on_sync_request
        self._create_widgets()
        self._refresh_list()

    def _create_widgets(self):
        # Add breakpoint frame
        add_frame = ttk.Frame(self)
        add_frame.pack(fill=tk.X, padx=2, pady=2)

        ttk.Label(add_frame, text="Expr:").pack(side=tk.LEFT)
        self.expr_entry = ttk.Entry(add_frame, width=16, font=("Courier", 9))
        self.expr_entry.pack(side=tk.LEFT, padx=2)
        self.expr_entry.bind("<Return>", lambda e: self._add_breakpoint())

        # Type selector (Exec, Write, Read, R/W)
        self.type_var = tk.StringVar(value=BreakpointManager.TYPE_EXECUTION)
        type_combo = ttk.Combobox(add_frame, textvariable=self.type_var,
                                   values=BreakpointManager.TYPES, width=7, state="readonly")
        type_combo.pack(side=tk.LEFT, padx=2)

        self.add_btn = ttk.Button(add_frame, text="Add", width=4, command=self._add_breakpoint)
        self.add_btn.pack(side=tk.LEFT, padx=2)

        ttk.Button(add_frame, text="Tog", width=3, command=self._toggle_selected).pack(side=tk.LEFT, padx=1)
        ttk.Button(add_frame, text="Edit", width=4, command=self._edit_selected).pack(side=tk.LEFT, padx=1)
        ttk.Button(add_frame, text="Del", width=3, command=self._delete_selected).pack(side=tk.LEFT, padx=1)
        ttk.Button(add_frame, text="Sync", width=4, command=self._request_sync).pack(side=tk.LEFT, padx=1)

        # Treeview for breakpoint list
        columns = ("enabled", "type", "address", "expression", "comment")
        self.tree = ttk.Treeview(self, columns=columns, show="headings", height=8)

        self.tree.heading("enabled", text="E")
        self.tree.heading("type", text="Type")
        self.tree.heading("address", text="Address")
        self.tree.heading("expression", text="Expression")
        self.tree.heading("comment", text="Comment")

        self.tree.column("enabled", width=25, anchor="center")
        self.tree.column("type", width=45, anchor="center")
        self.tree.column("address", width=90, anchor="w")
        self.tree.column("expression", width=100, anchor="w")
        self.tree.column("comment", width=120, anchor="w")

        # Scrollbar
        scrollbar = ttk.Scrollbar(self, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)

        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=2, pady=2)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y, pady=2)

        # Bind events
        self.tree.bind("<Double-1>", self._on_double_click)
        self.tree.bind("<space>", self._toggle_selected)

    def _add_breakpoint(self):
        """Add a new breakpoint"""
        expr = self.expr_entry.get().strip()
        if not expr:
            return

        try:
            bp_type = self.type_var.get()
            bp = self.manager.add(expr, bp_type=bp_type)
            self._refresh_list()
            self.expr_entry.delete(0, tk.END)
            self._request_sync()
        except ValueError as e:
            messagebox.showerror("Error", str(e))

    def _refresh_list(self):
        """Refresh the treeview from manager"""
        self.tree.delete(*self.tree.get_children())
        for bp in self.manager.breakpoints:
            enabled = "✓" if bp["enabled"] else ""
            gdb_status = f" (#{bp['gdb_num']})" if bp["gdb_num"] else ""
            address = f"0x{bp['address']:08X}{gdb_status}"
            # Get type label (handle old data without type)
            bp_type = bp.get("type", BreakpointManager.TYPE_EXECUTION)
            type_label = BreakpointManager.TYPE_LABELS.get(bp_type, "Exec")
            self.tree.insert("", tk.END, iid=str(bp["id"]), values=(
                enabled,
                type_label,
                address,
                bp["expression"],
                bp["comment"]
            ))

    def _get_selected_id(self):
        """Get selected breakpoint ID"""
        selection = self.tree.selection()
        if selection:
            return int(selection[0])
        return None

    def _toggle_selected(self, event=None):
        """Toggle selected breakpoint"""
        bp_id = self._get_selected_id()
        if bp_id:
            self.manager.toggle(bp_id)
            self._refresh_list()
            self._request_sync()

    def _edit_selected(self):
        """Edit selected breakpoint"""
        bp_id = self._get_selected_id()
        if not bp_id:
            return

        bp = self.manager.get_by_id(bp_id)
        if not bp:
            return

        # Create edit dialog
        dialog = tk.Toplevel(self)
        dialog.title("Edit Breakpoint")
        dialog.transient(self)
        dialog.grab_set()

        # Center dialog on parent window
        dialog_width = 400
        dialog_height = 180
        parent = self.winfo_toplevel()
        parent_x = parent.winfo_x()
        parent_y = parent.winfo_y()
        parent_width = parent.winfo_width()
        parent_height = parent.winfo_height()
        x = parent_x + (parent_width - dialog_width) // 2
        y = parent_y + (parent_height - dialog_height) // 2
        dialog.geometry(f"{dialog_width}x{dialog_height}+{x}+{y}")

        ttk.Label(dialog, text="Expression:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        expr_entry = ttk.Entry(dialog, width=30, font=("Courier", 9))
        expr_entry.grid(row=0, column=1, padx=5, pady=5, sticky="w")
        expr_entry.insert(0, bp["expression"])

        ttk.Label(dialog, text="Type:").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        type_var = tk.StringVar(value=bp.get("type", BreakpointManager.TYPE_EXECUTION))
        type_combo = ttk.Combobox(dialog, textvariable=type_var,
                                   values=BreakpointManager.TYPES, width=10, state="readonly")
        type_combo.grid(row=1, column=1, padx=5, pady=5, sticky="w")

        ttk.Label(dialog, text="Comment:").grid(row=2, column=0, padx=5, pady=5, sticky="w")
        comment_entry = ttk.Entry(dialog, width=30, font=("Courier", 9))
        comment_entry.grid(row=2, column=1, padx=5, pady=5, sticky="w")
        comment_entry.insert(0, bp["comment"])

        def save():
            try:
                new_expr = expr_entry.get().strip()
                new_type = type_var.get()
                new_comment = comment_entry.get().strip()
                if new_expr != bp["expression"]:
                    self.manager.update_expression(bp_id, new_expr)
                if new_type != bp.get("type", BreakpointManager.TYPE_EXECUTION):
                    self.manager.update_type(bp_id, new_type)
                if new_comment != bp["comment"]:
                    self.manager.update_comment(bp_id, new_comment)
                self._refresh_list()
                self._request_sync()
                dialog.destroy()
            except ValueError as e:
                messagebox.showerror("Error", str(e))

        btn_frame = ttk.Frame(dialog)
        btn_frame.grid(row=3, column=0, columnspan=2, pady=10)
        ttk.Button(btn_frame, text="Save", command=save).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frame, text="Cancel", command=dialog.destroy).pack(side=tk.LEFT, padx=5)

    def _delete_selected(self):
        """Delete selected breakpoint"""
        bp_id = self._get_selected_id()
        if bp_id:
            if messagebox.askyesno("Confirm", "Delete this breakpoint?"):
                self.manager.remove(bp_id)
                self._refresh_list()
                self._request_sync()

    def _on_double_click(self, event):
        """Handle double-click on treeview"""
        region = self.tree.identify("region", event.x, event.y)
        if region == "cell":
            column = self.tree.identify_column(event.x)
            if column == "#1":  # Enabled column (first column)
                self._toggle_selected()
            else:
                self._edit_selected()

    def _request_sync(self):
        """Request sync with GDB"""
        if self.on_sync_request:
            self.on_sync_request()

    def get_manager(self):
        """Get the breakpoint manager"""
        return self.manager

    def refresh(self):
        """Public method to refresh the list"""
        self._refresh_list()


class MemoryWatchManager:
    """Manages memory watches with file persistence"""

    # Size options
    SIZES = {
        "byte": 1,
        "word": 2,
        "dword": 4,
    }

    # Display format options
    FORMATS = ["hex", "dec", "signed"]

    def __init__(self, filepath=None):
        self.filepath = filepath or "memory_watches.json"
        self.watches = []
        self.next_id = 1
        self.load()

    def set_filepath(self, filepath):
        """Change filepath and reload"""
        self.filepath = filepath
        self.load()

    def load(self):
        """Load watches from file"""
        if os.path.exists(self.filepath):
            try:
                with open(self.filepath, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    self.watches = data.get("watches", [])
                    self.next_id = data.get("next_id", 1)
            except Exception as e:
                print(f"Failed to load memory watches: {e}")
                self.watches = []

    def save(self):
        """Save watches to file"""
        try:
            data = {
                "watches": self.watches,
                "next_id": self.next_id
            }
            with open(self.filepath, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
        except Exception as e:
            print(f"Failed to save memory watches: {e}")

    def add(self, expression, size="dword", fmt="hex", comment=""):
        """Add a new memory watch"""
        address = SafeExprEvaluator.evaluate(expression)
        watch = {
            "id": self.next_id,
            "expression": expression,
            "address": address,
            "size": size,
            "format": fmt,
            "comment": comment,
            "value": None,  # Current value (updated at runtime)
        }
        self.next_id += 1
        self.watches.append(watch)
        self.save()
        return watch

    def remove(self, watch_id):
        """Remove a watch by ID"""
        self.watches = [w for w in self.watches if w["id"] != watch_id]
        self.save()

    def update_comment(self, watch_id, comment):
        """Update watch comment"""
        for w in self.watches:
            if w["id"] == watch_id:
                w["comment"] = comment
                self.save()
                return w
        return None

    def update_expression(self, watch_id, expression):
        """Update watch expression"""
        address = SafeExprEvaluator.evaluate(expression)
        for w in self.watches:
            if w["id"] == watch_id:
                w["expression"] = expression
                w["address"] = address
                self.save()
                return w
        return None

    def update_size(self, watch_id, size):
        """Update watch size"""
        for w in self.watches:
            if w["id"] == watch_id:
                w["size"] = size
                self.save()
                return w
        return None

    def update_format(self, watch_id, fmt):
        """Update watch format"""
        for w in self.watches:
            if w["id"] == watch_id:
                w["format"] = fmt
                self.save()
                return w
        return None

    def set_value(self, watch_id, value):
        """Set the current value (runtime only, not saved)"""
        for w in self.watches:
            if w["id"] == watch_id:
                w["value"] = value
                return w
        return None

    def get_by_id(self, watch_id):
        """Get watch by ID"""
        for w in self.watches:
            if w["id"] == watch_id:
                return w
        return None

    def format_value(self, watch):
        """Format value for display"""
        if watch["value"] is None:
            return "?"

        value = watch["value"]
        size = watch["size"]
        fmt = watch["format"]

        if fmt == "hex":
            if size == "byte":
                return f"0x{value & 0xFF:02X}"
            elif size == "word":
                return f"0x{value & 0xFFFF:04X}"
            else:  # dword
                return f"0x{value & 0xFFFFFFFF:08X}"
        elif fmt == "dec":
            return str(value)
        elif fmt == "signed":
            if size == "byte":
                v = value & 0xFF
                if v >= 0x80:
                    v -= 0x100
            elif size == "word":
                v = value & 0xFFFF
                if v >= 0x8000:
                    v -= 0x10000
            else:  # dword
                v = value & 0xFFFFFFFF
                if v >= 0x80000000:
                    v -= 0x100000000
            return str(v)
        return str(value)


class MemoryWatchPanel(ttk.LabelFrame):
    """Panel to manage memory watches"""

    def __init__(self, parent, on_read_request=None, filepath=None):
        super().__init__(parent, text="Memory Watch")
        self.manager = MemoryWatchManager(filepath=filepath)
        self.on_read_request = on_read_request
        self._create_widgets()
        self._refresh_list()

    def _create_widgets(self):
        # Add watch frame
        add_frame = ttk.Frame(self)
        add_frame.pack(fill=tk.X, padx=2, pady=2)

        ttk.Label(add_frame, text="Expr:").pack(side=tk.LEFT)
        self.expr_entry = ttk.Entry(add_frame, width=16, font=("Courier", 9))
        self.expr_entry.pack(side=tk.LEFT, padx=2)
        self.expr_entry.bind("<Return>", lambda e: self._add_watch())

        # Size combo
        self.size_var = tk.StringVar(value="dword")
        size_combo = ttk.Combobox(add_frame, textvariable=self.size_var,
                                   values=["byte", "word", "dword"], width=5, state="readonly")
        size_combo.pack(side=tk.LEFT, padx=2)

        ttk.Button(add_frame, text="Add", width=4, command=self._add_watch).pack(side=tk.LEFT, padx=1)
        ttk.Button(add_frame, text="Edit", width=4, command=self._edit_selected).pack(side=tk.LEFT, padx=1)
        ttk.Button(add_frame, text="Del", width=3, command=self._delete_selected).pack(side=tk.LEFT, padx=1)
        ttk.Button(add_frame, text="Read", width=4, command=self._request_read).pack(side=tk.LEFT, padx=1)

        # Treeview for watch list
        columns = ("address", "size", "value", "comment")
        self.tree = ttk.Treeview(self, columns=columns, show="headings", height=6)

        self.tree.heading("address", text="Address")
        self.tree.heading("size", text="Size")
        self.tree.heading("value", text="Value")
        self.tree.heading("comment", text="Comment")

        self.tree.column("address", width=90, anchor="w")
        self.tree.column("size", width=50, anchor="center")
        self.tree.column("value", width=100, anchor="w")
        self.tree.column("comment", width=120, anchor="w")

        # Scrollbar
        scrollbar = ttk.Scrollbar(self, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)

        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=2, pady=2)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y, pady=2)

        # Bind events
        self.tree.bind("<Double-1>", self._on_double_click)

    def _add_watch(self):
        """Add a new memory watch"""
        expr = self.expr_entry.get().strip()
        if not expr:
            return

        try:
            watch = self.manager.add(expr, size=self.size_var.get())
            self._refresh_list()
            self.expr_entry.delete(0, tk.END)
            self._request_read()
        except ValueError as e:
            messagebox.showerror("Error", str(e))

    def _refresh_list(self):
        """Refresh the treeview from manager"""
        self.tree.delete(*self.tree.get_children())
        for w in self.manager.watches:
            value_str = self.manager.format_value(w)
            self.tree.insert("", tk.END, iid=str(w["id"]), values=(
                f"0x{w['address']:08X}",
                w["size"],
                value_str,
                w["comment"]
            ))

    def _get_selected_id(self):
        """Get selected watch ID"""
        selection = self.tree.selection()
        if selection:
            return int(selection[0])
        return None

    def _edit_selected(self):
        """Edit selected watch"""
        watch_id = self._get_selected_id()
        if not watch_id:
            return

        watch = self.manager.get_by_id(watch_id)
        if not watch:
            return

        # Create edit dialog
        dialog = tk.Toplevel(self)
        dialog.title("Edit Memory Watch")
        dialog.transient(self)
        dialog.grab_set()

        # Center dialog on parent window
        dialog_width = 400
        dialog_height = 180
        parent = self.winfo_toplevel()
        parent_x = parent.winfo_x()
        parent_y = parent.winfo_y()
        parent_width = parent.winfo_width()
        parent_height = parent.winfo_height()
        x = parent_x + (parent_width - dialog_width) // 2
        y = parent_y + (parent_height - dialog_height) // 2
        dialog.geometry(f"{dialog_width}x{dialog_height}+{x}+{y}")

        ttk.Label(dialog, text="Expression:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        expr_entry = ttk.Entry(dialog, width=30, font=("Courier", 9))
        expr_entry.grid(row=0, column=1, padx=5, pady=5, sticky="w")
        expr_entry.insert(0, watch["expression"])

        ttk.Label(dialog, text="Size:").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        size_var = tk.StringVar(value=watch["size"])
        size_combo = ttk.Combobox(dialog, textvariable=size_var,
                                   values=["byte", "word", "dword"], width=8, state="readonly")
        size_combo.grid(row=1, column=1, padx=5, pady=5, sticky="w")

        ttk.Label(dialog, text="Format:").grid(row=2, column=0, padx=5, pady=5, sticky="w")
        fmt_var = tk.StringVar(value=watch["format"])
        fmt_combo = ttk.Combobox(dialog, textvariable=fmt_var,
                                  values=["hex", "dec", "signed"], width=8, state="readonly")
        fmt_combo.grid(row=2, column=1, padx=5, pady=5, sticky="w")

        ttk.Label(dialog, text="Comment:").grid(row=3, column=0, padx=5, pady=5, sticky="w")
        comment_entry = ttk.Entry(dialog, width=30, font=("Courier", 9))
        comment_entry.grid(row=3, column=1, padx=5, pady=5, sticky="w")
        comment_entry.insert(0, watch["comment"])

        def save():
            try:
                new_expr = expr_entry.get().strip()
                new_size = size_var.get()
                new_fmt = fmt_var.get()
                new_comment = comment_entry.get().strip()

                if new_expr != watch["expression"]:
                    self.manager.update_expression(watch_id, new_expr)
                if new_size != watch["size"]:
                    self.manager.update_size(watch_id, new_size)
                if new_fmt != watch["format"]:
                    self.manager.update_format(watch_id, new_fmt)
                if new_comment != watch["comment"]:
                    self.manager.update_comment(watch_id, new_comment)

                self._refresh_list()
                self._request_read()
                dialog.destroy()
            except ValueError as e:
                messagebox.showerror("Error", str(e))

        btn_frame = ttk.Frame(dialog)
        btn_frame.grid(row=4, column=0, columnspan=2, pady=10)
        ttk.Button(btn_frame, text="Save", command=save).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frame, text="Cancel", command=dialog.destroy).pack(side=tk.LEFT, padx=5)

    def _delete_selected(self):
        """Delete selected watch"""
        watch_id = self._get_selected_id()
        if watch_id:
            if messagebox.askyesno("Confirm", "Delete this watch?"):
                self.manager.remove(watch_id)
                self._refresh_list()

    def _on_double_click(self, event):
        """Handle double-click on treeview"""
        self._edit_selected()

    def _request_read(self):
        """Request memory read for all watches"""
        if self.on_read_request:
            self.on_read_request()

    def get_manager(self):
        """Get the memory watch manager"""
        return self.manager

    def refresh(self):
        """Public method to refresh the list"""
        self._refresh_list()

    def update_value(self, watch_id, value):
        """Update a watch's value and refresh display"""
        self.manager.set_value(watch_id, value)
        self._refresh_list()


class GDBClientGUI:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("SH4/Dreamcast GDB Client")
        self.root.geometry("1200x800")

        self.profile_manager = ProfileManager()
        self.gdb = GDBClient()
        self.current_pc = None
        self.auto_refresh = tk.BooleanVar(value=True)
        self._pending_bp_ids = []  # Track pending breakpoint IDs for GDB number assignment
        self._pending_watch_reads = []  # Track pending memory watch reads
        self._target_running = False  # Track if target is running
        self._step_then_continue = False  # Flag for step-then-continue sequence
        self._sync_pending = False  # Flag for pending breakpoint sync (when target was running)

        self._create_menu()
        self._create_widgets()
        self._poll_output()
        self._update_status_display()
        self._update_title()

    def _create_menu(self):
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)

        # File menu
        file_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="File", menu=file_menu)
        file_menu.add_command(label="Connect", command=self._connect)
        file_menu.add_command(label="Disconnect", command=self._disconnect)
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=self._on_close)

        # Profile menu
        self.profile_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="Profile", menu=self.profile_menu)
        self._update_profile_menu()

        # View menu
        view_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="View", menu=view_menu)
        view_menu.add_checkbutton(label="Auto Refresh", variable=self.auto_refresh)

    def _create_widgets(self):
        # Main paned window
        main_paned = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main_paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Left panel - Registers and Breakpoints
        left_frame = ttk.Frame(main_paned)
        main_paned.add(left_frame, weight=1)

        self.reg_panel = RegisterPanel(left_frame)
        self.reg_panel.pack(fill=tk.X, padx=2, pady=2)

        self.bp_panel = BreakpointPanel(
            left_frame,
            on_sync_request=self._sync_breakpoints,
            filepath=self.profile_manager.get_breakpoints_file()
        )
        self.bp_panel.pack(fill=tk.X, padx=2, pady=2)

        self.watch_panel = MemoryWatchPanel(
            left_frame,
            on_read_request=self._read_watches,
            filepath=self.profile_manager.get_watches_file()
        )
        self.watch_panel.pack(fill=tk.BOTH, expand=True, padx=2, pady=2)

        # Right panel - Disassembly, Memory, Output
        right_frame = ttk.Frame(main_paned)
        main_paned.add(right_frame, weight=2)

        # Control buttons
        ctrl_frame = ttk.Frame(right_frame)
        ctrl_frame.pack(fill=tk.X, padx=2, pady=2)

        ttk.Button(ctrl_frame, text="Connect", command=self._connect, width=10).pack(side=tk.LEFT, padx=2)
        ttk.Button(ctrl_frame, text="Disconnect", command=self._disconnect, width=10).pack(side=tk.LEFT, padx=2)
        ttk.Separator(ctrl_frame, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        self.run_btn = ttk.Button(ctrl_frame, text="Continue", command=self._toggle_run, width=8)
        self.run_btn.pack(side=tk.LEFT, padx=2)
        self.step_btn = ttk.Button(ctrl_frame, text="Step", command=self._stepi, width=6)
        self.step_btn.pack(side=tk.LEFT, padx=2)
        self.next_btn = ttk.Button(ctrl_frame, text="Next", command=self._nexti, width=6)
        self.next_btn.pack(side=tk.LEFT, padx=2)
        ttk.Separator(ctrl_frame, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        ttk.Button(ctrl_frame, text="Refresh", command=self._refresh_all, width=8).pack(side=tk.LEFT, padx=2)

        # Status label
        self.status_var = tk.StringVar(value="Disconnected")
        ttk.Label(ctrl_frame, textvariable=self.status_var,
                 font=("Arial", 10, "bold")).pack(side=tk.RIGHT, padx=10)

        # Idle time indicator
        self.idle_label = ttk.Label(ctrl_frame, text="",
                                    font=("Arial", 9), foreground="gray")
        self.idle_label.pack(side=tk.RIGHT, padx=5)

        # Notebook for disasm/memory/output
        notebook = ttk.Notebook(right_frame)
        notebook.pack(fill=tk.BOTH, expand=True, padx=2, pady=2)

        # Disassembly tab
        self.disasm_panel = DisassemblyPanel(notebook, on_add_breakpoint=self._add_breakpoint_at)
        notebook.add(self.disasm_panel, text="Disassembly")

        # Memory tab
        self.mem_panel = MemoryPanel(notebook)
        notebook.add(self.mem_panel, text="Memory")
        self.mem_panel.dump_btn.configure(command=self._dump_memory)

        # Output tab
        output_frame = ttk.Frame(notebook)
        notebook.add(output_frame, text="GDB Output")

        self.output_text = scrolledtext.ScrolledText(
            output_frame, wrap=tk.WORD, font=("Courier", 9)
        )
        self.output_text.pack(fill=tk.BOTH, expand=True, padx=2, pady=2)

        # Command input
        cmd_frame = ttk.Frame(right_frame)
        cmd_frame.pack(fill=tk.X, padx=2, pady=2)

        ttk.Label(cmd_frame, text="GDB Command:").pack(side=tk.LEFT)
        self.cmd_entry = ttk.Entry(cmd_frame, font=("Courier", 10))
        self.cmd_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=5)
        self.cmd_entry.bind("<Return>", self._send_command)
        ttk.Button(cmd_frame, text="Send", command=self._send_command, width=8).pack(side=tk.LEFT)

    def _update_title(self):
        """Update window title with profile name"""
        profile = self.profile_manager.current_profile
        self.root.title(f"SH4/Dreamcast GDB Client - [{profile}]")

    def _update_profile_menu(self):
        """Update profile menu with available profiles"""
        self.profile_menu.delete(0, tk.END)

        # Add profile selection submenu
        profiles = self.profile_manager.list_profiles()
        current = self.profile_manager.current_profile

        for profile in profiles:
            label = f"● {profile}" if profile == current else f"  {profile}"
            self.profile_menu.add_command(
                label=label,
                command=lambda p=profile: self._switch_profile(p)
            )

        self.profile_menu.add_separator()
        self.profile_menu.add_command(label="New Profile...", command=self._create_profile)
        self.profile_menu.add_command(label="Rename Profile...", command=self._rename_profile)
        self.profile_menu.add_command(label="Delete Profile...", command=self._delete_profile)

    def _switch_profile(self, profile_name):
        """Switch to a different profile"""
        if profile_name == self.profile_manager.current_profile:
            return

        self.profile_manager.switch_profile(profile_name)

        # Update managers with new file paths
        self.bp_panel.get_manager().set_filepath(
            self.profile_manager.get_breakpoints_file()
        )
        self.bp_panel.refresh()

        self.watch_panel.get_manager().set_filepath(
            self.profile_manager.get_watches_file()
        )
        self.watch_panel.refresh()

        self._update_title()
        self._update_profile_menu()
        self._append_output(f"[Switched to profile: {profile_name}]\n")

        # Sync breakpoints if connected
        if self.gdb.connected:
            self._sync_breakpoints()

    def _create_profile(self):
        """Create a new profile"""
        name = simpledialog.askstring("New Profile", "Enter profile name:",
                                      parent=self.root)
        if name and name.strip():
            name = name.strip()
            if name in self.profile_manager.list_profiles():
                messagebox.showerror("Error", f"Profile '{name}' already exists")
                return
            self.profile_manager.create_profile(name)
            self._switch_profile(name)

    def _rename_profile(self):
        """Rename current profile"""
        current = self.profile_manager.current_profile
        if current == "default":
            messagebox.showerror("Error", "Cannot rename default profile")
            return

        name = simpledialog.askstring("Rename Profile",
                                      f"New name for '{current}':",
                                      parent=self.root)
        if name and name.strip():
            name = name.strip()
            if self.profile_manager.rename_profile(current, name):
                # Update managers with new file paths
                self.bp_panel.get_manager().set_filepath(
                    self.profile_manager.get_breakpoints_file()
                )
                self.watch_panel.get_manager().set_filepath(
                    self.profile_manager.get_watches_file()
                )
                self._update_title()
                self._update_profile_menu()
                self._append_output(f"[Renamed profile to: {name}]\n")
            else:
                messagebox.showerror("Error", f"Failed to rename profile")

    def _delete_profile(self):
        """Delete current profile"""
        current = self.profile_manager.current_profile
        if current == "default":
            messagebox.showerror("Error", "Cannot delete default profile")
            return

        if messagebox.askyesno("Confirm",
                              f"Delete profile '{current}'?\nThis will delete all breakpoints and watches."):
            self.profile_manager.delete_profile(current)
            # Switch to default
            self._switch_profile("default")
            self._update_profile_menu()

    def _update_status_display(self):
        """Update idle time display periodically"""
        if self.gdb.connected:
            idle = self.gdb.get_idle_time()
            if idle > 5:
                self.idle_label.configure(text=f"Idle: {idle:.0f}s", foreground="orange")
            else:
                self.idle_label.configure(text=f"Idle: {idle:.0f}s", foreground="gray")
        else:
            self.idle_label.configure(text="", foreground="gray")

        # Schedule next update
        self.root.after(1000, self._update_status_display)

    def _connect(self):
        self._append_output("[Connecting to 127.0.0.1:3263...]\n")
        self.status_var.set("Connecting...")
        self.gdb.start("127.0.0.1:3263")

    def _disconnect(self):
        self.gdb.stop()
        self._append_output("[Disconnected]\n")
        self.status_var.set("Disconnected")
        # Clear GDB numbers
        self.bp_panel.get_manager().clear_gdb_nums()
        self.bp_panel.refresh()

    def _stepi(self):
        self.gdb.stepi()

    def _nexti(self):
        self.gdb.nexti()

    def _toggle_run(self):
        """Toggle between Continue and Interrupt based on state"""
        if self._target_running:
            self.gdb.interrupt()
        else:
            self.gdb.continue_exec()

    def _update_button_states(self):
        """Update button states based on target running state"""
        if self._target_running:
            self.run_btn.configure(text="Interrupt")
            self.step_btn.configure(state=tk.DISABLED)
            self.next_btn.configure(state=tk.DISABLED)
        else:
            self.run_btn.configure(text="Continue")
            self.step_btn.configure(state=tk.NORMAL)
            self.next_btn.configure(state=tk.NORMAL)

    def _refresh_all(self):
        """Refresh registers, disassembly, breakpoints, and watches"""
        self.gdb.get_registers()
        self.gdb.list_breakpoints()
        if self.current_pc:
            self.gdb.disassemble(self.current_pc, 30)
        # Read memory watches
        self._read_watches()

    def _dump_memory(self):
        if self._target_running:
            self._append_output("[Cannot read memory while running]\n")
            return
        addr = self.mem_panel.get_address()
        self.gdb.read_memory(addr, 256)

    def _add_breakpoint_at(self, addr):
        """Add breakpoint at address (called from disassembly panel)"""
        expr = f"0x{addr:08X}"
        try:
            self.bp_panel.get_manager().add(expr)
            self.bp_panel.refresh()
            self._sync_breakpoints()
            self._append_output(f"[Added breakpoint at {expr}]\n")
        except ValueError as e:
            self._append_output(f"[Error adding breakpoint: {e}]\n")

    def _sync_breakpoints(self):
        """Sync breakpoints with GDB (handles running target)"""
        if not self.gdb.connected:
            return

        # If target is running, interrupt it first and queue the sync
        if self._target_running:
            self._append_output("[Interrupting target to sync breakpoints...]\n")
            self._sync_pending = True
            self.gdb.interrupt()
            return

        # Target is stopped, do the actual sync
        self._do_sync_breakpoints()

    def _do_sync_breakpoints(self):
        """Actually sync breakpoints with GDB (target must be stopped)"""
        if not self.gdb.connected:
            return

        manager = self.bp_panel.get_manager()

        # First, delete all GDB breakpoints
        self.gdb.send_command("delete breakpoints")

        # Clear GDB numbers
        manager.clear_gdb_nums()

        # Set enabled breakpoints
        exec_count = 0
        watch_count = 0
        for bp in manager.get_enabled():
            bp_type = bp.get("type", BreakpointManager.TYPE_EXECUTION)
            if bp_type == BreakpointManager.TYPE_EXECUTION:
                self.gdb.set_breakpoint(bp["address"])
                exec_count += 1
            elif bp_type == BreakpointManager.TYPE_WRITE:
                self.gdb.set_watchpoint(bp["address"], "write")
                watch_count += 1
            elif bp_type == BreakpointManager.TYPE_READ:
                self.gdb.set_watchpoint(bp["address"], "read")
                watch_count += 1
            elif bp_type == BreakpointManager.TYPE_ACCESS:
                self.gdb.set_watchpoint(bp["address"], "access")
                watch_count += 1
            # Store pending bp_id to match with GDB response
            self._pending_bp_ids.append(bp["id"])

        total = exec_count + watch_count
        self._append_output(f"[Synced {total} breakpoints ({exec_count} exec, {watch_count} watch) to GDB]\n")

    def _read_watches(self):
        """Read memory for all watches"""
        if not self.gdb.connected:
            return

        # Don't read memory while target is running
        if self._target_running:
            return

        manager = self.watch_panel.get_manager()
        for watch in manager.watches:
            size_bytes = MemoryWatchManager.SIZES.get(watch["size"], 4)
            # Use MI command to read memory
            # -data-read-memory addr x word_size rows cols
            self.gdb._send_raw(f"-data-read-memory 0x{watch['address']:X} x 1 1 {size_bytes}")
            self._pending_watch_reads.append(watch["id"])

    def _on_breakpoint_created(self, gdb_num, address=None):
        """Called when GDB creates a breakpoint or watchpoint"""
        if self._pending_bp_ids:
            bp_id = self._pending_bp_ids.pop(0)
            manager = self.bp_panel.get_manager()
            manager.set_gdb_num(bp_id, gdb_num)
            self.bp_panel.refresh()

    def _send_command(self, event=None):
        cmd = self.cmd_entry.get().strip()
        if cmd:
            self._append_output(f"(gdb) {cmd}\n")
            self.gdb.send_command(cmd)
            self.cmd_entry.delete(0, tk.END)

    def _append_output(self, text):
        self.output_text.insert(tk.END, text)
        self.output_text.see(tk.END)

    def _parse_mi_output(self, line):
        """Parse GDB/MI output and update GUI"""
        # Notify watchdog of response (any line starting with ^ or * is a response)
        if line.startswith('^') or line.startswith('*') or line.startswith('='):
            self.gdb.notify_response()

        # Connection status
        if "^connected" in line:
            self.status_var.set("Connected")
            self._refresh_all()
            # Sync breakpoints after connection
            self.root.after(500, self._sync_breakpoints)
        elif "^error" in line:
            if "Remote connection closed" in line:
                self.status_var.set("Disconnected")
                # Clear GDB numbers on disconnect
                self.bp_panel.get_manager().clear_gdb_nums()
                self.bp_panel.refresh()

        # Stop notification
        if "*stopped" in line:
            self._target_running = False
            self._update_button_states()

            # Extract PC from *stopped message if available
            # Format: *stopped,...,frame={addr="0x8c010000",...}
            frame_match = re.search(r'frame=\{[^}]*addr="(0x[0-9a-fA-F]+)"', line)
            if frame_match:
                try:
                    new_pc = int(frame_match.group(1), 16)
                    self.current_pc = new_pc
                except:
                    pass

            # Check if this is part of step-then-continue sequence
            if self._step_then_continue:
                self._step_then_continue = False
                # Continue execution after stepping over the breakpoint
                self.root.after(10, self.gdb.continue_exec)
                return  # Don't refresh yet, we're continuing

            # Check if we have a pending breakpoint sync
            if self._sync_pending:
                self._sync_pending = False
                # Perform the actual sync now that target is stopped
                self._do_sync_breakpoints()
                # Resume execution
                self.root.after(10, self.gdb.continue_exec)
                return  # Don't refresh yet, we're continuing

            self.status_var.set("Stopped")
            if self.auto_refresh.get():
                self._refresh_all()

        # Running notification
        if "*running" in line:
            self._target_running = True
            self.status_var.set("Running")
            self._update_button_states()

        # Register values
        if "^done,register-values=" in line:
            self._parse_registers(line)

        # Disassembly
        if "^done,asm_insns=" in line:
            self._parse_disassembly(line)

        # Memory - check if it's for a watch or for memory dump panel
        # Format: ^done,addr="...",nr-bytes="...",memory=[{addr="...",data=["0x00",...]}]
        if "memory=[" in line and "^done," in line:
            if self._pending_watch_reads:
                self._parse_watch_memory(line)
            else:
                self._parse_memory(line)

        # Breakpoint created (^done,bkpt={...}) or Watchpoint (^done,wpt={...} or hw-...wpt)
        if "^done,bkpt=" in line or "^done,wpt=" in line or "^done,hw-" in line:
            self._parse_new_breakpoint(line)

        # Breakpoint list
        if "BreakpointTable" in line:
            self._parse_breakpoints(line)

        # Console output (show in output panel)
        if line.startswith("~"):
            # Remove MI prefix and unescape
            text = line[1:].strip()
            if text.startswith('"') and text.endswith('"'):
                text = text[1:-1].replace("\\n", "\n").replace("\\t", "\t")
            self._append_output(text)

    def _parse_registers(self, line):
        """Parse register values from MI output"""
        reg_data = {}
        # Pattern: {number="0",value="0x..."}
        pattern = r'\{number="(\d+)",value="([^"]+)"\}'
        for match in re.finditer(pattern, line):
            num = int(match.group(1))
            value = match.group(2)
            if num < len(RegisterPanel.REG_NAMES):
                name = RegisterPanel.REG_NAMES[num]
                reg_data[name] = value

                # Track PC for disassembly
                if name == "pc":
                    try:
                        self.current_pc = int(value, 16)
                        # Auto-update disassembly
                        if self.auto_refresh.get():
                            self.gdb.disassemble(self.current_pc, 30)
                    except:
                        pass

        self.reg_panel.update_registers(reg_data)

    def _parse_disassembly(self, line):
        """Parse disassembly from MI output"""
        lines = []
        # Pattern: {address="0x...",inst="..."}
        pattern = r'\{address="([^"]+)"[^}]*inst="([^"]+)"\}'
        for match in re.finditer(pattern, line):
            addr = int(match.group(1), 16)
            instr = match.group(2)
            lines.append((addr, "", instr))

        self.disasm_panel.update_disassembly(lines, self.current_pc)

    def _parse_watch_memory(self, line):
        """Parse memory read for watches"""
        if not self._pending_watch_reads:
            return

        watch_id = self._pending_watch_reads.pop(0)
        watch = self.watch_panel.get_manager().get_by_id(watch_id)
        if not watch:
            return

        # GDB/MI format: memory=[{addr="0x...",data=["0x01","0x00",...]}]
        # Extract data array from the line
        data_match = re.search(r'data=\[([^\]]+)\]', line)
        if data_match:
            # Extract all hex values: "0x01","0x00",...
            values = re.findall(r'"(0x[0-9a-fA-F]+)"', data_match.group(1))
            if values:
                # Combine bytes based on size (little endian)
                result = 0
                for i, v in enumerate(values):
                    try:
                        byte_val = int(v, 16)
                        result |= (byte_val << (i * 8))
                    except:
                        pass
                self.watch_panel.update_value(watch_id, result)

    def _parse_memory(self, line):
        """Parse memory dump from MI output"""
        data = []
        addr = self.mem_panel.get_address()
        # Pattern: {addr="0x...",data=["0x...",...]}
        pattern = r'data=\[([^\]]+)\]'
        match = re.search(pattern, line)
        if match:
            values = re.findall(r'"([^"]+)"', match.group(1))
            for v in values:
                try:
                    data.append(int(v, 16))
                except:
                    pass

        self.mem_panel.update_memory(addr, data)

    def _parse_new_breakpoint(self, line):
        """Parse newly created breakpoint or watchpoint from MI output"""
        # Pattern for breakpoint: ^done,bkpt={number="1",addr="0x..."}
        # Pattern for watchpoint: ^done,wpt={number="1",exp="*0x..."}
        # Also handles hw-awpt, hw-rwpt
        gdb_num = None

        # Try to find breakpoint
        bkpt_match = re.search(r'bkpt=\{([^}]+)\}', line)
        if bkpt_match:
            bp_data = {}
            for kv in re.finditer(r'(\w+)="([^"]*)"', bkpt_match.group(1)):
                bp_data[kv.group(1)] = kv.group(2)

            if "number" in bp_data:
                gdb_num = int(bp_data["number"])
                if "addr" in bp_data:
                    try:
                        address = int(bp_data["addr"], 16)
                        self._on_breakpoint_created(gdb_num, address)
                        return
                    except ValueError:
                        pass

        # Try to find watchpoint (wpt, hw-awpt, hw-rwpt)
        wpt_match = re.search(r'(?:wpt|hw-awpt|hw-rwpt)=\{([^}]+)\}', line)
        if wpt_match:
            wp_data = {}
            for kv in re.finditer(r'(\w+)="([^"]*)"', wpt_match.group(1)):
                wp_data[kv.group(1)] = kv.group(2)

            if "number" in wp_data:
                gdb_num = int(wp_data["number"])
                # For watchpoints, we don't always have an address in the response
                self._on_breakpoint_created(gdb_num, None)

    def _parse_breakpoints(self, line):
        """Parse breakpoint list from MI output"""
        # We now manage breakpoints locally, so just log this
        pass

    def _poll_output(self):
        """Poll for GDB output and update GUI"""
        try:
            while True:
                line = self.gdb.output_queue.get_nowait()
                self._parse_mi_output(line)

                # Also show raw MI output for debugging
                if not line.startswith("(gdb)"):
                    self._append_output(line)
        except queue.Empty:
            pass

        # Schedule next poll
        self.root.after(50, self._poll_output)

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()

    def _on_close(self):
        self.gdb.stop()
        self.root.destroy()


if __name__ == "__main__":
    app = GDBClientGUI()
    app.run()
