#!/usr/bin/env python3
"""
Experiment runner for distributed system benchmarks.
Automates compilation and execution of experiments with optional dry-run mode.
"""

import argparse
import os
import subprocess
import sys
import time


class ExperimentRunner:
    def __init__(self, dry_run=False, ssh_user="weihai", use_sshpass=False):
        self.dry_run = dry_run
        self.ssh_user = ssh_user
        self.use_sshpass = use_sshpass
        self.commands = []
        self.experiment_params = {}
        
    def run_command(self, cmd, description=""):
        """Execute a command and always log to commands list"""
        if description:
            print(f"# {description}")
            self.commands.append(f"# {description}")
        
        self.commands.append(cmd)
        
        if self.dry_run:
            print(f"[DRY RUN] {cmd}")
        else:
            print(f"Running: {cmd}")
            result = subprocess.run(cmd, shell=True, capture_output=False)
            if result.returncode != 0:
                print(f"Command failed with return code {result.returncode}")
                return False
        return True

    def add_section_break(self, title=""):
        """Add a section break for better readability"""
        if title:
            self.commands.append(f"\n# === {title} ===")
        else:
            self.commands.append("")

    def compile_project(self, shards, is_replicated, is_micro):
        """Compile the project with specified parameters"""
        print(f"\n=== COMPILATION ===")
        print(f"Shards: {shards}, Replicated: {is_replicated}, Micro: {is_micro}")
        
        self.add_section_break("COMPILATION")
        
        # Cleanup previous builds
        self.run_command("rm -rf ./out-perf.masstree/*", "Clean out-perf.masstree")
        self.run_command("rm -rf ./src/mako/out-perf.masstree/*", "Clean mako out-perf.masstree")
        self.add_section_break()
        
        # Clean CMake cache
        self.run_command("mkdir -p build", "mkdir build directory")
        self.run_command("cd build", "Change to build directory")
        self.run_command("rm -rf *", "Clean CMake cache")
        self.add_section_break()
        
        # Configure with CMake
        cmake_cmd = (f"cmake ..")
        self.run_command(cmake_cmd, "Configure with CMake")
        self.add_section_break()
        
        # Build targets
        targets = ["dbtest"] #"basic", "paxos_async_commit_test", "erpc_client", "erpc_server"]
        for target in targets:
            if not self.run_command(f"make -j32 {target} VERBOSE=1", f"Build {target}"):
                return False
        
        self.add_section_break()
        self.run_command("cd ..", "Return to root directory")
        
        # Note: We don't actually change directories in the script generation
        # This is just for the generated script readability
        return True

    def get_host_for_role(self, shard_index, role):
        """Get the host IP for a given shard and role"""
        config_file = f"bash/shard{shard_index}.config.pub"
        try:
            with open(config_file, 'r') as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) == 2 and parts[0] == role:
                        return parts[1]
        except FileNotFoundError:
            print(f"Config file {config_file} not found")
        return "127.0.0.1"  # fallback

    def run_experiment(self, shards, threads, is_replicated, is_micro):
        """Run experiment with or without replication"""
        mode = "REPLICATED" if is_replicated else "NON-REPLICATED"
        workload = "MICRO" if is_micro else "TPCC"
        print(f"\n=== {mode} EXPERIMENT ({workload}) ===")
        print(f"Shards: {shards}, Threads: {threads}, Workload: {workload}")
        
        self.add_section_break(f"{mode} EXPERIMENT")
        
        # Create results directory
        self.run_command("mkdir -p results", "Create results directory")
        self.add_section_break()
        
        if is_replicated:
            roles = ["localhost", "learner", "p2", "p1"]
        else:
            roles = ["localhost"]
        
        for shard in range(shards):
            self.add_section_break(f"SHARD {shard}")
            
            for role in roles:
                host = self.get_host_for_role(shard, role)
                # Pass is_micro and is_replicated as 5th and 6th parameters
                micro_flag = "1" if is_micro else "0"
                replicated_flag = "1" if is_replicated else "0"
                cmd = f"bash bash/shard.sh {shards} {shard} {threads} {role} {micro_flag} {replicated_flag}"
                log_file = f"./results/{role}-{shard}.log"
                
                # Always use SSH for consistency
                if self.use_sshpass:
                    # Create a wrapper command that handles sudo password
                    wrapper_cmd = f"export SUDO_ASKPASS=/bin/false; echo '{os.environ.get('SSHPASS', '')}' | sudo -S -v 2>/dev/null; {cmd}"
                    ssh_cmd = f"sshpass -e ssh -o StrictHostKeyChecking=no {self.ssh_user}@{host} 'cd {os.getcwd()} && {wrapper_cmd}'"
                else:
                    ssh_cmd = f"ssh {self.ssh_user}@{host} 'cd {os.getcwd()} && {cmd}'"
                
                full_cmd = f"nohup {ssh_cmd} > {log_file} 2>&1 &"
                
                self.run_command(full_cmd, f"Start shard {shard} role {role}")
                
                if is_replicated:
                    if not self.dry_run:
                        time.sleep(0.2)
                    else:
                        self.run_command("sleep 0.2", "Wait between role starts")
            
            # Add extra spacing between shards
            if shard < shards - 1:
                self.add_section_break()

    def cleanup(self, shards, is_replicated):
        """Cleanup processes and log files on all shard servers"""
        print(f"\n=== CLEANUP ===")
        
        self.add_section_break("CLEANUP")
        
        self.run_command("rm -f results/*.log", "Remove log files")
        self.run_command("rm -f nfs_sync_*", "Remove NFS sync files")
        self.add_section_break()
        
        # Kill processes on all shard servers
        if is_replicated:
            roles = ["p1", "p2", "localhost", "learner"]
        else:
            roles = ["localhost"]
        
        for shard in range(shards):
            self.add_section_break(f"CLEANUP SHARD {shard}")
            
            for role in roles:
                host = self.get_host_for_role(shard, role)
                # Try regular kill first, then sudo kill if needed
                kill_cmd = 'ps aux | grep -i dbtest | awk "{print \\$2}" | xargs kill -9 2>/dev/null || ps aux | grep -i dbtest | awk "{print \\$2}" | xargs sudo kill -9 2>/dev/null || true'
                
                if self.use_sshpass:
                    # Prepare sudo for the session
                    wrapper_kill_cmd = f"echo '{os.environ.get('SSHPASS', '')}' | sudo -S -v 2>/dev/null; {kill_cmd}"
                    ssh_kill_cmd = f"sshpass -e ssh -o StrictHostKeyChecking=no {self.ssh_user}@{host} '{wrapper_kill_cmd}'"
                else:
                    ssh_kill_cmd = f"ssh {self.ssh_user}@{host} '{kill_cmd}'"
                
                self.run_command(ssh_kill_cmd, f"Kill dbtest processes on shard {shard} role {role} ({host})")

    def generate_filename(self, is_cleanup=False, only_compile=False, skip_compile=False):
        """Generate filename based on experiment parameters"""
        params = self.experiment_params
        shards = params.get('shards', 0)
        replicated = "repl" if params.get('is_replicated', False) else "norepl"
        micro = "micro" if params.get('is_micro', False) else "tpcc"
        threads = params.get('threads', 0)
        
        suffix = ""
        if is_cleanup:
            suffix = "_del"
        elif only_compile:
            suffix = "_compile-only"
        elif skip_compile:
            suffix = "_no-compile"
        
        return f"experiment_s{shards}_{replicated}_{micro}_t{threads}{suffix}.sh"

    def save_commands_script(self, filename=None, is_cleanup=False, only_compile=False, skip_compile=False):
        """Save all commands to a bash script with descriptive header"""
        if not self.commands:
            return
            
        if filename is None:
            filename = self.generate_filename(is_cleanup=is_cleanup, only_compile=only_compile, skip_compile=skip_compile)
        
        # Create header comment
        params = self.experiment_params
        header = f"""#!/bin/bash
#
# Distributed System Experiment Script
# Generated on: {time.strftime('%Y-%m-%d %H:%M:%S')}
#
# Experiment Configuration:
#   - Shards: {params.get('shards', 'N/A')}
#   - Replication: {'Enabled (Paxos)' if params.get('is_replicated', False) else 'Disabled'}
#   - Workload: {'Micro-benchmark' if params.get('is_micro', False) else 'TPC-C'}
#   - Threads per shard: {params.get('threads', 'N/A')}
#   - Mode: {'Dry run' if self.dry_run else 'Executed'}
#
# This script performs the following steps:
# 1. Compilation: Clean build directory and compile with CMake
# 2. Execution: Run distributed benchmark across configured shards
# 3. Results: Log outputs to ./results/ directory
#
# Usage: ./{filename}
#

"""
        
        script_content = header + "\n".join(self.commands)
        with open(filename, 'w') as f:
            f.write(script_content)
        os.chmod(filename, 0o755)
        mode = "Dry run" if self.dry_run else "Executed"
        print(f"\n{mode} commands saved to {filename}")


def main():
    parser = argparse.ArgumentParser(description="Run distributed system experiments")
    
    # Compilation parameters
    parser.add_argument("--shards", type=int, default=3, 
                       help="Number of shards (default: 3)")
    parser.add_argument("--replicated", action="store_true", 
                       help="Enable replication (Paxos)")
    parser.add_argument("--micro", action="store_true",
                       help="Run micro-benchmark instead of TPCC")
    
    parser.add_argument("--threads", type=int, default=6, 
                       help="Number of worker threads per shard (default: 6)")
    
    # Mode selection
    parser.add_argument("--dry-run", action="store_true", 
                       help="Generate bash script without executing")
    parser.add_argument("--cleanup-only", action="store_true",
                       help="Only run cleanup (kill processes and remove logs)")
    parser.add_argument("--skip-compile", action="store_true",
                       help="Skip compilation phase and only run experiment")
    parser.add_argument("--only-compile", action="store_true",
                       help="Only compile the project without running experiments")
    
    # SSH options
    parser.add_argument("--ssh-user", type=str, default="weihai",
                       help="SSH username (default: weihai)")
    parser.add_argument("--use-sshpass", action="store_true", default=True,
                       help="Use sshpass for SSH authentication (default: True, requires SSHPASS env var)")
    parser.add_argument("--no-sshpass", action="store_true",
                       help="Disable sshpass and use regular SSH")
    
    args = parser.parse_args()
    
    # Handle sshpass logic
    use_sshpass = args.use_sshpass and not args.no_sshpass
    
    runner = ExperimentRunner(dry_run=args.dry_run, ssh_user=args.ssh_user, use_sshpass=use_sshpass)
    
    try:
        # Store experiment parameters for filename generation
        runner.experiment_params = {
            'shards': args.shards,
            'is_replicated': args.replicated,
            'is_micro': args.micro,
            'threads': args.threads,
        }
        
        if args.cleanup_only:
            runner.cleanup(args.shards, args.replicated)
        elif args.only_compile:
            # Only compile, don't run experiments
            success = runner.compile_project(args.shards, 
                                           1 if args.replicated else 0,
                                           1 if args.micro else 0)
            if not success and not args.dry_run:
                print("Compilation failed!")
                return 1
        else:
            # Compile first (unless skipped), then run
            if not args.skip_compile:
                success = runner.compile_project(args.shards, 
                                               1 if args.replicated else 0,
                                               1 if args.micro else 0)
                if not success and not args.dry_run:
                    print("Compilation failed!")
                    return 1
            
            runner.run_experiment(args.shards, args.threads, args.replicated, args.micro)
        
        # Always save commands to file
        runner.save_commands_script(is_cleanup=args.cleanup_only, 
                                  only_compile=args.only_compile,
                                  skip_compile=args.skip_compile)
            
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        return 1
    except Exception as e:
        print(f"Error: {e}")
        return 1
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
