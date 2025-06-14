import json
import os

# ── Default Configuration Values ──
mem_trace = []
debug = False
output_dir = 'out/'

# Number of virtual threads (general)
NUM_THREADS = 4
SIZE_KEY = 4
SIZE_INT = 4
SIZE_WEIGHT = 4
SIZE_FEAT = 2

# Tensor Regions
I_BASE = 0x10000000
TILE_BASE = I_BASE  # Alias, will be updated if I_BASE changes
QK_BASE = 0x20000000
QI_BASE = 0x30000000
QO_BASE = 0x40000000
PIV_BASE = 0x50000000
KM_BASE = 0x60000000
WO_BASE = 0x80000000
IV_BASE = 0x100000000
GM_BASE = 0x800000000
WV_BASE = 0xF00000000

# GEMM Parameters
GEMM_ALIGNMENT = 4
GEMM_WT_GROUP = 2
GEMM_SIZE = 4

# GATHER PARAMETERS
NUM_TILES_GATHER = 4  # Renamed from NUM_TILES to distinguish if needed, maps to JSON NUM_TILES
TILE_FEATS_GATHER = 16 # Renamed from TILE_FEATS, maps to JSON TILE_FEATS
BULK_FEATS_GATHER = 4  # Renamed from BULK_FEATS, maps to JSON BULK_FEATS
N_THREADS_GATHER = 1   # This is the gather-specific thread count
TOTAL_FEATS_PT_GATHER = NUM_TILES_GATHER * TILE_FEATS_GATHER # Default calculation

# New parameter from C++ config
NUM_PIVOTS = 2 # Default value

# --- Load Configuration from JSON ---
_current_dir = os.path.dirname(os.path.abspath(__file__))
_config_json_path = os.path.join(_current_dir, "config.json")

try:
    with open(_config_json_path, 'r') as f:
        _config_data = json.load(f)
    print(f"Successfully loaded configuration from {_config_json_path}")

    # Helper for hex string to int conversion
    def _hex_to_int(value, default_val):
        if isinstance(value, str) and value.startswith("0x"):
            try:
                return int(value, 16)
            except ValueError:
                print(f"Warning: Could not parse hex string '{value}'. Using default {hex(default_val)}.")
                return default_val
        elif isinstance(value, int):
            return value
        print(f"Warning: Unexpected type for hex value '{value}'. Using default {hex(default_val)}.")
        return default_val

    # Override defaults with values from JSON
    debug = _config_data.get("debug", debug)
    output_dir = _config_data.get("output_dir", output_dir)
    NUM_THREADS = _config_data.get("NUM_THREADS", NUM_THREADS)
    SIZE_KEY = _config_data.get("SIZE_KEY", SIZE_KEY)
    SIZE_INT = _config_data.get("SIZE_INT", SIZE_INT)
    SIZE_WEIGHT = _config_data.get("SIZE_WEIGHT", SIZE_WEIGHT)

    I_BASE = _hex_to_int(_config_data.get("I_BASE"), I_BASE)
    TILE_BASE = I_BASE  # Maintain alias
    QK_BASE = _hex_to_int(_config_data.get("QK_BASE"), QK_BASE)
    QI_BASE = _hex_to_int(_config_data.get("QI_BASE"), QI_BASE)
    QO_BASE = _hex_to_int(_config_data.get("QO_BASE"), QO_BASE)
    PIV_BASE = _hex_to_int(_config_data.get("PIV_BASE"), PIV_BASE)
    KM_BASE = _hex_to_int(_config_data.get("KM_BASE"), KM_BASE)
    WO_BASE = _hex_to_int(_config_data.get("WO_BASE"), WO_BASE)
    IV_BASE = _hex_to_int(_config_data.get("IV_BASE"), IV_BASE)
    WV_BASE = _hex_to_int(_config_data.get("WV_BASE"), WV_BASE)

    GEMM_ALIGNMENT = _config_data.get("GEMM_ALIGNMENT", GEMM_ALIGNMENT)
    GEMM_WT_GROUP = _config_data.get("GEMM_WT_GROUP", GEMM_WT_GROUP)
    GEMM_SIZE = _config_data.get("GEMM_SIZE", GEMM_SIZE)
    
    # Mapping JSON NUM_TILES, TILE_FEATS, BULK_FEATS to the gather-specific variables
    NUM_TILES_GATHER = _config_data.get("NUM_TILES", NUM_TILES_GATHER)
    TILE_FEATS_GATHER = _config_data.get("TILE_FEATS", TILE_FEATS_GATHER)
    BULK_FEATS_GATHER = _config_data.get("BULK_FEATS", BULK_FEATS_GATHER)
    N_THREADS_GATHER = _config_data.get("N_THREADS_GATHER", N_THREADS_GATHER)
    
    # TOTAL_FEATS_PT can be loaded or calculated using updated gather values
    TOTAL_FEATS_PT_GATHER = _config_data.get("TOTAL_FEATS_PT", NUM_TILES_GATHER * TILE_FEATS_GATHER)
    NUM_PIVOTS = _config_data.get("NUM_PIVOTS", NUM_PIVOTS)

except FileNotFoundError:
    print(f"Warning: Configuration file '{_config_json_path}' not found. Using default values.")
except json.JSONDecodeError:
    print(f"Warning: Error decoding JSON from '{_config_json_path}'. Using default values.")
except Exception as e:
    print(f"Warning: An unexpected error occurred while loading configuration: {e}. Using default values.")

# For compatibility with scripts that might expect these specific names from the old minuet_config.py
# These were the names used under the "GATHER PARAMETERS" section
NUM_TILES = NUM_TILES_GATHER
TILE_FEATS = TILE_FEATS_GATHER
BULK_FEATS = BULK_FEATS_GATHER
N_THREADS = N_THREADS_GATHER # For gather simulation if it uses N_THREADS
TOTAL_FEATS_PT = TOTAL_FEATS_PT_GATHER

# Clean up temporary variables from global namespace (optional)
# These are prefixed with _ so they are less likely to cause issues if not deleted.
# del _current_dir, _config_json_path, _config_data, _hex_to_int

# Ensure all imported names are clear
# Example: if minuet_trace.py uses "from minuet_config import *", it gets all these globals.
# The general NUM_THREADS (e.g., for C++ phases) is distinct from N_THREADS (for Python gather simulation).

