#include "minuet_map.hpp"
#include <algorithm>
#include <cmath> // For std::ceil in progress reporting
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>
#include <numeric> // For std::accumulate
#include <thread> // For std::thread
#include <mutex>  // For std::mutex
#include <atomic> // For std::atomic

// --- Global Variable Definitions ---
std::vector<MemoryAccessEntry> mem_trace; // Updated name
std::string curr_phase = "";              // Updated name

// --- Mutexes for threaded operations ---
static std::mutex kmap_update_mutex;
static std::mutex global_mem_trace_mutex;

// --- Getter/Setter for global state and mem_trace management ---
std::vector<MemoryAccessEntry> get_mem_trace() {
    return mem_trace;
}

void clear_mem_trace() {
    mem_trace.clear();
}

void set_curr_phase(const std::string& phase_name) {
    curr_phase = phase_name;
}

std::string get_curr_phase() {
    return curr_phase;
}

void set_debug_flag(bool debug_val) {
    g_config.debug = debug_val; // Set in global config
}

bool get_debug_flag() {
    return g_config.debug; // Get from global config
}



// Global constant maps (matching Python names for clarity)
// Using the bidict class defined in the header
bidict<std::string, int>
    PHASES({{"RDX", 0}, {"QRY", 1}, {"SRT", 2}, {"PVT", 3}, {"LKP", 4}, {"GTH", 5}, {"SCT", 6}});

bidict<std::string, int> TENSORS({
    {"I", 0},
    {"QK", 1},
    {"QI", 2},
    {"QO", 3},
    {"PIV", 4},
    {"KM", 5},
    {"WC", 6},
    {"TILE", 7}, // TILE is I_BASE, handled in addr_to_tensor
    {"IV", 8}, // IV_BASE is not in TENSORS, handled as string
    {"GM", 9}, // GM_BASE is not in TENSORS, handled as string
    {"WV", 10}, // WV_BASE is not in TENSORS, handled as string
    {"Unknown", 255} // Default case for unknown tensors
});




bidict<std::string, int> OPS({{"R", 0}, {"W", 1}});




// --- Memory Tracing Functions ---
uint8_t addr_to_tensor(uint64_t addr) { // Renamed and return type changed
  if (addr >= g_config.I_BASE && addr < g_config.QK_BASE)
    return TENSORS.forward.at("I");
  if (addr >= g_config.QK_BASE && addr < g_config.QI_BASE)
    return TENSORS.forward.at("QK");
  if (addr >= g_config.QI_BASE && addr < g_config.QO_BASE)
    return TENSORS.forward.at("QI");
  if (addr >= g_config.QO_BASE && addr < g_config.PIV_BASE)
    return TENSORS.forward.at("QO");
  if (addr >= g_config.PIV_BASE && addr < g_config.KM_BASE)
    return TENSORS.forward.at("PIV");
  if (addr >= g_config.KM_BASE && addr < g_config.WO_BASE)
    return TENSORS.forward.at("KM");
  if (addr >= g_config.WO_BASE && addr < g_config.IV_BASE) // Assuming WO_BASE is for "WC" (Weight Cache/Compatible)
    return TENSORS.forward.at("WC");
  if (addr >= g_config.IV_BASE && addr < g_config.GM_BASE)
    return TENSORS.forward.at("IV");
  if (addr >= g_config.GM_BASE && addr < g_config.WV_BASE)
    return TENSORS.forward.at("GM");
  // Ensure WV_BASE range check is appropriate, e.g., WV_BASE + some_size
  // Using a large enough range for WV as an example.
  if (addr >= g_config.WV_BASE && (addr < g_config.WV_BASE + (2ULL << 32))) // Example range for WV
    return TENSORS.forward.at("WV");
  
  return TENSORS.forward.at("Unknown"); // Default if no range matches
}

std::string addr_to_tensor_str(uint64_t addr) {
    uint8_t tensor_id = addr_to_tensor(addr);
    try {
        return TENSORS.inverse.at(tensor_id);
    } catch (const std::out_of_range& oor) {
        return "Unknown"; // Fallback if ID is not in inverse map somehow
    }
}


uint32_t write_gmem_trace(const std::string &filename, int sizeof_addr /* = 4 */) { // Added sizeof_addr parameter
  gzFile outFile = gzopen(filename.c_str(), "wb");
  if (!outFile) {
    throw std::runtime_error("Failed to open file for writing: " + filename);
  }

  if (sizeof_addr != 4 && sizeof_addr != 8) {
    gzclose(outFile);
    throw std::invalid_argument("sizeof_addr must be 4 or 8, got: " + std::to_string(sizeof_addr));
  }

  uLong crc = crc32(0L, Z_NULL, 0);
  auto write_and_crc = [&](const void* data, unsigned int len) {
      if (gzwrite(outFile, data, len) != static_cast<int>(len)) {
          gzclose(outFile);
          throw std::runtime_error("Failed to write data to gzip file during gmem trace.");
      }
      crc = crc32(crc, static_cast<const Bytef*>(data), len);
  };

  uint32_t num_entries = static_cast<uint32_t>(mem_trace.size());
  write_and_crc(&num_entries, sizeof(num_entries));

  for (const auto &entry : mem_trace) {
    // Directly use the uint8_t fields from MemoryAccessEntry
    uint8_t phase_val = entry.phase;
    uint8_t tid_val = entry.thread_id;
    uint8_t op_val = entry.op;
    uint8_t tensor_val = entry.tensor;
    // Cast address to uint32_t for writing, as per original compressed format
    // uint32_t addr_val = static_cast<uint32_t>(entry.addr); 

    write_and_crc(&phase_val, sizeof(phase_val));
    write_and_crc(&tid_val, sizeof(tid_val));
    write_and_crc(&op_val, sizeof(op_val));
    write_and_crc(&tensor_val, sizeof(tensor_val));
    // write_and_crc(&addr_val, sizeof(addr_val));
    if (sizeof_addr == 4) {
        uint32_t addr_val_32 = static_cast<uint32_t>(entry.addr);
        write_and_crc(&addr_val_32, sizeof(addr_val_32));
    } else { // sizeof_addr == 8
        uint64_t addr_val_64 = entry.addr;
        write_and_crc(&addr_val_64, sizeof(addr_val_64));
    }
  }
  gzclose(outFile);

  std::cout << "Memory trace written to " << filename << std::endl;
  std::cout << "Collected " << mem_trace.size() << " entries" << std::endl; 
  return static_cast<uint32_t>(crc);
}

void clear_global_mem_trace() {
    mem_trace.clear();
}

void record_access(int thread_id, const std::string &op_str, uint64_t addr) {
  std::lock_guard<std::mutex> lock(global_mem_trace_mutex);
  uint8_t phase_id = PHASES.forward.at(curr_phase);
  uint8_t op_id = OPS.forward.at(op_str);
  uint8_t tensor_id = addr_to_tensor(addr); // Use the new function returning uint8_t
  
  mem_trace.push_back({phase_id, static_cast<uint8_t>(thread_id), op_id, tensor_id, addr});
}

// --- Algorithm Phases ---
// Radix Sort (Simplified: only records accesses, actual sort not fully
// implemented for brevity) Matches Python's radix_sort_with_memtrace which
// focuses on memory access patterns.
std::vector<uint32_t> radix_sort_with_memtrace(std::vector<uint32_t> &arr,
                                               uint64_t base_addr) {
  const int passes = 4;
  size_t N = arr.size();
  if (N == 0)
    return arr;

  // std::vector<uint32_t> aux(N); // Conceptually for sorting

  for (int p = 0; p < passes; ++p) {
    // Simulate the part of radix sort that builds the auxiliary array.
    // Python's version:
    // for i in range(N - 1, -1, -1):
    //     record_access(t_id, "R", base + i*SIZE_KEY) // First read
    //     val = arr[i]
    //     # ... logic to get byte_val and target aux_idx using counts ...
    //     record_access(t_id, "R", base + i*SIZE_KEY) // Second read (of arr[i]
    //     / val) aux[aux_idx] = val record_access(t_id, 'W', base +
    //     aux_idx*SIZE_KEY) // Write to aux

    // We simulate N elements being processed.
    for (size_t i = 0; i < N; ++i) {
      int t_id = static_cast<int>(i % g_config.NUM_THREADS);
      // First read of an element from arr
      record_access(t_id,
        "R", base_addr + i * g_config.SIZE_KEY); // "R"
    }
    for (size_t i = 0; i < N; ++i) {
      int t_id = static_cast<int>(i % g_config.NUM_THREADS);
      // Second read of the same element from arr
      record_access(t_id, 
        "R", base_addr + i * g_config.SIZE_KEY); // "R"

      // Write to auxiliary array (simulated position)
      // In a real sort, this write would be to aux[calculated_pos]
      // For simulation, we record a write to a conceptual 'aux' region,
      // using 'i' as a proxy for calculated_pos to ensure N writes.
      // The base address for aux is assumed to be the same as arr for this
      // trace.
      record_access(t_id, OPS.inverse.at(1), base_addr + i * g_config.SIZE_KEY); // "W"
    }
    // arr, aux = aux, arr // Conceptually, data is swapped or copied back
    // If arr is swapped with aux, the next pass reads from what was aux.
    // The base address remains 'base_addr' for tracing purposes of this logical
    // array.
  }
  return arr;
}

std::vector<IndexedCoord>
compute_unique_sorted_coords(const std::vector<Coord3D> &in_coords,
                             int stride) {
  curr_phase = PHASES.inverse.at(0); // "RDX"

  std::vector<std::pair<uint32_t, int>>
      idx_keys_pairs; // Stores (key, original_index)
  idx_keys_pairs.reserve(in_coords.size());

  for (size_t idx = 0; idx < in_coords.size(); ++idx) {
    const auto &coord = in_coords[idx];
    // Python: record_access(idx % NUM_THREADS, 'W', I_BASE + idx * SIZE_KEY)
    // This write is for the initial list of idx_keys before sorting.
    // Let's assume I_BASE is the start of a conceptual array for these packed
    // keys. record_access(static_cast<int>(idx % NUM_THREADS),
    // OPS.inverse.at(1), g_config.I_BASE + idx * g_config.SIZE_KEY); // "W"
    Coord3D qtz_coord = coord.quantized(stride);
    idx_keys_pairs.emplace_back(qtz_coord.to_key(), static_cast<int>(idx));
  }

  // Extract raw keys for memory trace simulation of radix sort
  std::vector<uint32_t> raw_keys;
  raw_keys.reserve(idx_keys_pairs.size());
  for (const auto &pair : idx_keys_pairs) {
    raw_keys.push_back(pair.first);
  }
  // The base address for radix sort in Python is I_BASE.
  radix_sort_with_memtrace(raw_keys, g_config.I_BASE);

  // Sort idx_keys_pairs by key, preserving original index for tie-breaking
  // (std::stable_sort if needed) Python's `sorted(idx_keys, key=lambda item:
  // item[0])` is stable.
  std::stable_sort(
      idx_keys_pairs.begin(), idx_keys_pairs.end(),
      [](const auto &a, const auto &b) { return a.first < b.first; });

  std::vector<IndexedCoord> uniq_coords_vec; // Renamed from uniq_coords
  if (idx_keys_pairs.empty()) {
    return uniq_coords_vec;
  }

  uint32_t last_key = idx_keys_pairs[0].first;
  int orig_idx_from_input = idx_keys_pairs[0].second;
  uniq_coords_vec.emplace_back(Coord3D::from_key(last_key),
                               orig_idx_from_input);

  for (size_t i = 1; i < idx_keys_pairs.size(); ++i) {
    if (idx_keys_pairs[i].first != last_key) {
      last_key = idx_keys_pairs[i].first;
      orig_idx_from_input = idx_keys_pairs[i].second;
      uniq_coords_vec.emplace_back(Coord3D::from_key(last_key),
                                   orig_idx_from_input);
    }
  }

  if (g_config.debug) {
    std::cout << "Unique sorted coordinates (count: " << uniq_coords_vec.size()
              << ")" << std::endl;
    for (const auto &ic : uniq_coords_vec) {
      std::cout << "  Key: " << to_hex_string(ic.to_key())
                << ", Coord: " << ic.coord << ", Orig Idx: " << ic.orig_idx
                << std::endl;
    }
  }
  return uniq_coords_vec;
}

BuildQueriesResult
build_coordinate_queries(const std::vector<IndexedCoord> &uniq_coords,
                         int stride, // stride is not used in python version
                         const std::vector<Coord3D> &off_coords) {
  curr_phase = PHASES.inverse.at(1); // "QRY"
  size_t num_inputs = uniq_coords.size();
  size_t num_offsets = off_coords.size();
  size_t total_queries = num_inputs * num_offsets;

  BuildQueriesResult result;
  result.qry_keys.resize(total_queries);
  result.qry_in_idx.resize(total_queries);
  result.qry_off_idx.resize(total_queries);
  result.wt_offsets.resize(
      total_queries); // Python uses [None] * total_queries initially

  for (size_t off_idx = 0; off_idx < num_offsets; ++off_idx) {
    const auto &offset_val = off_coords[off_idx]; // dx, dy, dz
    for (size_t in_idx = 0; in_idx < num_inputs; ++in_idx) {
      const auto &indexed_coord_item = uniq_coords[in_idx];
      size_t glob_idx = off_idx * num_inputs + in_idx;

      // Python version does not record accesses in this function.
      // Removing record_access calls.
      // record_access(static_cast<int>(glob_idx % NUM_THREADS),
      // "R", g_config.I_BASE + in_idx * g_config.SIZE_KEY); // "R"

      Coord3D qk_coord = indexed_coord_item.coord + offset_val;
      result.qry_keys[glob_idx] =
          IndexedCoord(qk_coord, indexed_coord_item.orig_idx);
      result.qry_in_idx[glob_idx] = static_cast<int>(in_idx);
      result.qry_off_idx[glob_idx] = static_cast<int>(off_idx);
      result.wt_offsets[glob_idx] = offset_val; // Store the Coord3D offset

      // Removing record_access calls for writes.
      // record_access(static_cast<int>(glob_idx % NUM_THREADS),
      // OPS.inverse.at(1), g_config.QK_BASE + glob_idx * g_config.SIZE_KEY);    // "W" for
      // qry_key record_access(static_cast<int>(glob_idx % NUM_THREADS),
      // OPS.inverse.at(1), g_config.QI_BASE + glob_idx * g_config.SIZE_INT);    // "W" for
      // qry_in_idx record_access(static_cast<int>(glob_idx % NUM_THREADS),
      // OPS.inverse.at(1), g_config.QO_BASE + glob_idx * g_config.SIZE_INT);    // "W" for
      // qry_off_idx record_access(static_cast<int>(glob_idx % NUM_THREADS),
      // OPS.inverse.at(1), g_config.WO_BASE + glob_idx * g_config.SIZE_WEIGHT); // "W" for
      // wt_offset (packed key)
    }
  }
  return result;
}

TilesPivotsResult
create_tiles_and_pivots(const std::vector<IndexedCoord> &uniq_coords,
                        int tile_size_param) // Renamed from tile_size to avoid
                                             // conflict with local var
{
  curr_phase = PHASES.inverse.at(3); // "PVT"
  TilesPivotsResult result;
  int current_tile_size = tile_size_param;

  if (uniq_coords.empty()) {
    if (g_config.debug)
      std::cout << "Skipping tile creation, no unique coordinates."
                << std::endl;
    return result;
  }

  if (current_tile_size <= 0) { // Python: if tile_size is None or <= 0
    current_tile_size = static_cast<int>(uniq_coords.size());
    if (current_tile_size == 0) { // Still could be zero if uniq_coords was
                                  // empty and handled above, but defensive
      if (g_config.debug)
        std::cout << "Tile size is zero, cannot create tiles." << std::endl;
      return result; // No tiles can be made
    }
    if (g_config.debug)
      std::cout << "Tile size not specified or invalid, using full range: "
                << current_tile_size << std::endl;
  }

  for (size_t start = 0; start < uniq_coords.size();
       start += current_tile_size) {
    size_t end = std::min(start + current_tile_size, uniq_coords.size());
    std::vector<IndexedCoord> current_tile;
    current_tile.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
      current_tile.push_back(uniq_coords[i]);
   }
    result.tiles.push_back(current_tile);

    // Pivot selection (simplified: first element of the tile)
    if (!current_tile.empty()) {
      result.pivots.push_back(current_tile[0]);
      // Write pivot to PIV_BASE (simulated)
      // Python: record_access(start % NUM_THREADS, 'W', PIV_BASE + (result.pivots.size()-1) * SIZE_KEY)
      record_access(0, "W", g_config.PIV_BASE + (result.pivots.size() - 1) * g_config.SIZE_KEY); // "W"
    }
  }

  if (g_config.debug) {
    std::cout << "Created " << result.tiles.size() << " tiles and "
              << result.pivots.size() << " pivots." << std::endl;
    // for (size_t i = 0; i < result.tiles.size(); ++i) {
    //     std::cout << "  Tile " << i << " (size " << result.tiles[i].size() << "): ";
    //     for (const auto& ic : result.tiles[i]) {
    //         std::cout << ic.coord << " (orig_idx " << ic.orig_idx << "), ";
    //     }
    //     std::cout << std::endl;
    // }
    // for (size_t i = 0; i < result.pivots.size(); ++i) {
    //     std::cout << "  Pivot " << i << ": " << result.pivots[i].coord << " (orig_idx " << result.pivots[i].orig_idx << ")" << std::endl;
    // }
  }
  return result;
}
KernelMapType perform_coordinate_lookup(
    const std::vector<IndexedCoord> &uniq_coords,
    const std::vector<IndexedCoord> &qry_keys,
    const std::vector<int> &qry_in_idx, const std::vector<int> &qry_off_idx,
    const std::vector<Coord3D> &wt_offsets, // wt_offsets is not directly used in Python lookup logic for kmap values
    const std::vector<std::vector<IndexedCoord>> &tiles,
    const std::vector<IndexedCoord> &pivs, int tile_size_param) {

    set_curr_phase(PHASES.inverse.at(4)); // "LKP"
    KernelMapType kmap(false); // false for descending order (longest match list first)
    
    if (uniq_coords.empty() || qry_keys.empty()) {
        return kmap;
    }

    std::atomic<uint64_t> kmap_write_idx_atomic{0}; // Atomic counter for KM write simulation

    const size_t qry_count = qry_keys.size();
    const int BATCH_SIZE = 128; // As in Python
    const size_t num_batches = (qry_count + BATCH_SIZE - 1) / BATCH_SIZE;
    unsigned int num_hw_threads = g_config.NUM_THREADS; // Use configured NUM_THREADS

    std::cout << "Starting LKP phase with " << num_hw_threads << " threads, "
              << num_batches << " batches." << std::endl;

    for (size_t batch_idx = 0; batch_idx < num_batches; ++batch_idx) {
        size_t batch_start = batch_idx * BATCH_SIZE;
        size_t current_batch_size = std::min(static_cast<size_t>(BATCH_SIZE), qry_count - batch_start);

        if (current_batch_size <= 0) break;

        std::vector<std::thread> threads_pool;
        size_t portion_size = (current_batch_size + num_hw_threads - 1) / num_hw_threads;

        for (unsigned int tid = 0; tid < num_hw_threads; ++tid) {
            size_t thread_start_in_batch = tid * portion_size;
            size_t thread_end_in_batch = std::min(thread_start_in_batch + portion_size, current_batch_size);

            if (thread_start_in_batch < current_batch_size) {
                threads_pool.emplace_back([&, batch_start, tid, thread_start_in_batch, thread_end_in_batch]() {
                    std::vector<MemoryAccessEntry> local_thread_mem_trace;
                    auto record_access_local = 
                        [&](int t_id, const std::string& op_str, uint64_t addr_val) {
                        uint8_t phase_id = PHASES.forward.at(get_curr_phase());
                        uint8_t op_id = OPS.forward.at(op_str);
                        uint8_t tensor_id = addr_to_tensor(addr_val);
                        local_thread_mem_trace.push_back({phase_id, static_cast<uint8_t>(t_id), op_id, tensor_id, addr_val});
                    };

                    for (size_t qry_offset_in_batch = thread_start_in_batch; qry_offset_in_batch < thread_end_in_batch; ++qry_offset_in_batch) {
                        size_t q_glob_idx = batch_start + qry_offset_in_batch;
                        if (q_glob_idx >= qry_count) continue;

                        const auto &q_key_item = qry_keys[q_glob_idx];
                        uint32_t current_query_key = q_key_item.to_key();
                        int query_original_src_idx = q_key_item.orig_idx; // Original index of the source point that generated this query
                        int current_query_offset_list_idx = qry_off_idx[q_glob_idx]; // Index into the original off_coords list

                        // 1. Read query key
                        record_access_local(tid, 
                          "R", // "R"
                                      g_config.QK_BASE + q_glob_idx * g_config.SIZE_KEY);

                        // 2. Simulate Python's find_tile_id (binary search on pivs)
                        int target_tile_id = -1;
                        if (!pivs.empty()) {
                            int low = 0, high = static_cast<int>(pivs.size()) - 1;
                            target_tile_id = 0; 
                            while (low <= high) {
                                int mid = low + (high - low) / 2;
                                record_access_local(tid, 
                                  "R", // "R" pivot
                                              g_config.PIV_BASE + mid * g_config.SIZE_KEY);
                                if (pivs[mid].to_key() <= current_query_key) {
                                    target_tile_id = mid;
                                    low = mid + 1;
                                } else {
                                    high = mid - 1;
                                }
                            }
                        }
                        
                        // 3. Simulate Python's search_in_tile
                        if (target_tile_id != -1 && target_tile_id < static_cast<int>(tiles.size())) {
                            const auto &current_tile_vec = tiles[target_tile_id];
                            // Python version does linear scan in tile; C++ can do binary search if tile is sorted
                            // For now, matching Python's linear scan for trace consistency
                            for (size_t local_idx_in_tile = 0; local_idx_in_tile < current_tile_vec.size(); ++local_idx_in_tile) {
                                const auto& tile_indexed_coord = current_tile_vec[local_idx_in_tile];
                                size_t approx_tile_element_orig_idx = static_cast<size_t>(target_tile_id * tile_size_param + local_idx_in_tile);
                                if (approx_tile_element_orig_idx >= uniq_coords.size()) {
                                     approx_tile_element_orig_idx = uniq_coords.empty() ? 0 : uniq_coords.size() - 1;
                                }
                                record_access_local(tid, 
                                  "R", // "R" from TILE
                                              g_config.TILE_BASE + approx_tile_element_orig_idx * g_config.SIZE_KEY);

                                if (tile_indexed_coord.to_key() == current_query_key) {
                                    // Match found
                                    int input_idx_from_uniq_coords = tile_indexed_coord.orig_idx; // This is the original index from the initial input coordinates

                                    { // Lock kmap for update
                                        std::lock_guard<std::mutex> lock(kmap_update_mutex);
                                        kmap[current_query_offset_list_idx].emplace_back(input_idx_from_uniq_coords, query_original_src_idx);
                                    }
                                    
                                    // Record write to kernel map simulation
                                    uint64_t current_kmap_write_offset = kmap_write_idx_atomic.fetch_add(1);
                                    record_access_local(tid, OPS.inverse.at(1), // "W"
                                                  g_config.KM_BASE + current_kmap_write_offset * g_config.SIZE_INT);
                                    break; 
                                }
                            }
                        }
                    } // end for qry_offset_in_batch

                    // Merge local trace to global trace
                    if (!local_thread_mem_trace.empty()) {
                        std::lock_guard<std::mutex> lock(global_mem_trace_mutex);
                        mem_trace.insert(mem_trace.end(), local_thread_mem_trace.begin(), local_thread_mem_trace.end());
                    }
                }); // end lambda
            } // end if thread_start_in_batch
        } // end for tid

        for (auto &th : threads_pool) {
            if (th.joinable()) {
                th.join();
            }
        }
        if ((batch_idx + 1) % 10 == 0 || (batch_idx + 1) == num_batches) { // Print progress
             std::cout << "LKP Progress: Batch " << (batch_idx + 1) << "/" << num_batches << " processed." << std::endl;
        }
    } // end for batch_idx
    
    set_curr_phase(""); // Clear phase
    std::cout << "LKP phase complete." << std::endl;
    return kmap;
}

uint32_t write_kernel_map_to_gz(
    const KernelMapType &kmap_data, const std::string &filename,
    const std::vector<Coord3D>
        &off_list 
) {
  gzFile outFile = gzopen(filename.c_str(), "wb");
  if (!outFile) {
    throw std::runtime_error("Failed to open file for writing: " + filename);
  }

  uLong crc = crc32(0L, Z_NULL, 0);
  auto write_and_crc = [&](const void* data, unsigned int len) {
      if (gzwrite(outFile, data, len) != static_cast<int>(len)) {
          gzclose(outFile);
          throw std::runtime_error("Failed to write data to gzip file during kernel map writing.");
      }
      crc = crc32(crc, static_cast<const Bytef*>(data), len);
  };

  // Calculate total number of entries (pairs) across all offsets
  uint32_t num_total_entries = 0;
  for (const auto &pair : kmap_data) {
    num_total_entries += static_cast<uint32_t>(pair.second.size());
  }

  // if (gzwrite(outFile, &num_total_entries, sizeof(num_total_entries)) !=
  //     sizeof(num_total_entries)) {
  //   gzclose(outFile);
  //   throw std::runtime_error(
  //       "Failed to write number of entries to kernel map gzip file.");
  // }
  write_and_crc(&num_total_entries, sizeof(num_total_entries));

  // Iterate through the map (sorted by offset_idx due to std::map)
  // For SortedByValueSizeMap, we need to get the items in its sorted order if that's desired for writing.
  // However, the Python version writes based on iterating kmap.items(), which for a standard dict
  // is insertion order (or arbitrary in older Pythons). For SortedByValueLengthDict, it's value-length order.
  // The C++ std::map iterates by key. If we want to write in value-size-sorted order:
  auto sorted_items = kmap_data.get_sorted_items();
  for (const auto &pair : sorted_items) { // Iterate sorted items
    uint32_t offset_idx = pair.first; // This is the integer index for off_list

    if (offset_idx >= off_list.size()) {
        std::cerr << "Error in write_kernel_map_to_gz: offset_idx " << offset_idx 
                  << " is out of bounds for off_list (size " << off_list.size() 
                  << "). Skipping this kmap entry." << std::endl;
        continue;
    }
    const Coord3D& actual_offset_coord = off_list[offset_idx];
    uint32_t packed_offset_key_to_write = actual_offset_coord.to_key(); 

    const auto &matches =
        pair.second; // vector of (input_idx, query_src_orig_idx)

    for (const auto &match : matches) {
      uint32_t input_idx = static_cast<uint32_t>(match.first);
      uint32_t query_src_orig_idx = static_cast<uint32_t>(match.second);

      // Write: packed_offset_key_to_write, input_idx, query_src_orig_idx
      // if (gzwrite(outFile, &packed_offset_key_to_write, sizeof(packed_offset_key_to_write)) !=
      //         sizeof(packed_offset_key_to_write) ||
      //     gzwrite(outFile, &input_idx, sizeof(input_idx)) !=
      //         sizeof(input_idx) ||
      //     gzwrite(outFile, &query_src_orig_idx, sizeof(query_src_orig_idx)) !=
      //         sizeof(query_src_orig_idx)) {
      //   gzclose(outFile);
      //   throw std::runtime_error(
      //       "Failed to write kernel map entry to gzip file.");
      // }
      write_and_crc(&packed_offset_key_to_write, sizeof(packed_offset_key_to_write));
      write_and_crc(&input_idx, sizeof(input_idx));
      write_and_crc(&query_src_orig_idx, sizeof(query_src_orig_idx));
    }
  }

  gzclose(outFile);
  std::cout << "Kernel map successfully written to " << filename << " with "
            << num_total_entries << " entries." << std::endl;

            return static_cast<uint32_t>(crc);
}

// --- New Structs and Functions for Greedy Grouping ---

// Helper to convert host uint32_t to big-endian network byte order
// uint32_t hton_u32(uint32_t val) { // DEFINITION REMOVED
//     return htonl(val);
// }

// DEFINITIONS for GemmInfo, GroupInfo, GreedyGroupResult structs are in minuet_gather.hpp (declarations)
// Their definitions (if they were more than just structs, e.g. with methods) or the functions below are moved.

// uint32_t write_gemm_list_cpp(const std::vector<GemmInfo>& gemm_data_list, const std::string& filename) { // DEFINITION REMOVED
// ...implementation moved to minuet_gather.cpp...
// }

// GreedyGroupResult greedy_group_cpp( // DEFINITION REMOVED
//     const std::vector<int>& slots,
//     int alignment,
//     int max_group_items,
//     int max_raw_slots
// ) {
// ...implementation moved to minuet_gather.cpp...
// }