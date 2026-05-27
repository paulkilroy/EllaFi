Import("env")

def do_deploy(source, target, env):
    import subprocess, sys, os
    project_dir = env.subst("$PROJECT_DIR")
    pio_env = env.subst("$PIOENV")

    print("Building filesystem image...")
    r = subprocess.run(
        [sys.executable, "-m", "platformio", "run", "-t", "buildfs", "-e", pio_env],
        cwd=project_dir
    )
    if r.returncode != 0:
        Exit(r.returncode)

    r = subprocess.run([sys.executable, "scripts/deploy.py"], cwd=project_dir)
    if r.returncode != 0:
        Exit(r.returncode)

env.Replace(UPLOADCMD=do_deploy)
