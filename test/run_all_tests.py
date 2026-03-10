#!/usr/bin/env python3
"""Orchestrates all test files."""

import sys
import asyncio
import argparse
import subprocess
from pathlib import Path


def run_stress_test(host, port, clients, duration, start_ip=None, start_mac=None):
    """Run general concurrent client stress test."""
    print("\n" + "="*60)
    print("TIER 1: Concurrent Client Stress Test")
    print("="*60)
    
    cmd = ["python3", "test/stress_test.py", "--host", host, "--port", str(port), 
           "--clients", str(clients), "--duration", str(duration)]
    if start_ip:
        cmd.extend(["--start-ip", start_ip])
    if start_mac:
        cmd.extend(["--start-mac", start_mac])
    
    result = subprocess.run(cmd, cwd=Path(__file__).parent.parent)
    return result.returncode == 0


async def run_happy_path_test(host, port, fake_mac=None, fake_ip=None):
    """Run happy path validation test."""
    print("\n" + "="*60)
    print("TIER 1.5: Happy Path Validation")
    print("="*60)

    from stress_test_happy_path import run_happy_path_test
    result = await run_happy_path_test(host, port, fake_mac=fake_mac, fake_ip=fake_ip)
    return result


async def run_race_conditions_test(host, port):
    """Run all race condition tests."""
    print("\n" + "="*60)
    print("TIER 2-6: Race Condition Tests")
    print("="*60)
    
    from stress_test_race_conditions import run_all_race_condition_tests
    result = await run_all_race_condition_tests(host, port)
    return result


async def main():
    parser = argparse.ArgumentParser(
        description="EllaFi comprehensive test suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run all tests
  python3 test/run_all_tests.py --host localhost
  
  # Happy path only
  python3 test/run_all_tests.py --host localhost --test happy-path
  
  # Race conditions only
  python3 test/run_all_tests.py --host localhost --test race-conditions
  
  # Stress test with 50 concurrent clients
  python3 test/run_all_tests.py --host localhost --test stress --clients 50
        """
    )
    
    parser.add_argument("--host", default="ellafi.local", help="server host (default: ellafi.local)")
    parser.add_argument("--port", type=int, default=80, help="server port (default 80)")
    parser.add_argument("--test", choices=["all", "stress", "happy-path", "race-conditions"], 
                       default="all", help="which test tier to run (default: all)")
    parser.add_argument("--clients", type=int, default=10, 
                       help="number of concurrent clients for stress test (default 10)")
    parser.add_argument("--duration", type=int, default=10, 
                       help="duration per client in stress test (default 10s)")
    parser.add_argument("--start-ip", help="first client IP passed as query param (e.g. 10.0.0.2)")
    parser.add_argument("--start-mac", help="first MAC address for stress test (e.g. 00:11:22:33:44:00)")
    parser.add_argument("--mac", help="client MAC for happy path test (optional)")
    parser.add_argument("--ip", help="client IP for happy path test (optional)")
    
    args = parser.parse_args()

    results = {"stress": None, "happy-path": None, "race-conditions": None}


    try:
        if args.test in ["all", "stress"]:
            results["stress"] = run_stress_test(
                args.host, args.port, args.clients, args.duration, 
                args.start_ip, args.start_mac
            )
        
        if args.test in ["all", "happy-path"]:
            results["happy-path"] = await run_happy_path_test(
                args.host, args.port, fake_mac=args.mac, fake_ip=args.ip)
        
        if args.test in ["all", "race-conditions"]:
            results["race-conditions"] = await run_race_conditions_test(args.host, args.port)
        
        # Summary
        print("\n" + "="*60)
        print("Test Suite Summary")
        print("="*60)
        
        active_tests = {k: v for k, v in results.items() if v is not None}
        passed = sum(1 for v in active_tests.values() if v)
        total = len(active_tests)
        
        for test_name, result in active_tests.items():
            status = "✓ PASS" if result else "✗ FAIL"
            print(f"  {status}: {test_name}")
        
        print(f"\nOverall: {passed}/{total} test suites passed")
        
        exit(0 if passed == total else 1)
    
    except KeyboardInterrupt:
        print("\n\nTests interrupted by user")
        exit(1)
    except Exception as e:
        print(f"\n\nTest suite error: {e}")
        import traceback
        traceback.print_exc()
        exit(1)


if __name__ == "__main__":
    asyncio.run(main())
