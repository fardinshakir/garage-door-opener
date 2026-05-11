import os

Import("env")

def load_env():
    env_path = os.path.join(env["PROJECT_DIR"], ".env")
    if not os.path.exists(env_path):
        print("WARNING: .env file not found — copy .env.example to .env and fill it in")
        return

    with open(env_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, val = line.split("=", 1)
            key = key.strip()
            val = val.strip().strip('"').strip("'")
            env.Append(CPPDEFINES=[(key, '\\"' + val + '\\"')])

    print("Loaded .env")

load_env()
