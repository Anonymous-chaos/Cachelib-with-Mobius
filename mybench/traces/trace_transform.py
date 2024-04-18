import struct

keys = set()
req_count = 0

# Function to convert a CSV trace file to a binary trace file
def csv_to_binary(csv_file, binary_file):
    with open(csv_file, 'r') as csv_input:
        with open(binary_file, 'ab') as binary_output:
            global req_count
            global keys
            # Define the format string for packing the binary data
            s = struct.Struct("<IQIq")
            # Iterate over each line in the CSV file
            for line in csv_input:
                # Split the line into Object ID and other parameters
                ############################# Choose one command ################################
                object_id = line.strip().split(',')[0] # csv for kv-1
                # object_id = line.strip().split(',')[1] # csv for kv-2, cdn, and block
                #################################################################################
                try:
                    req_count += 1
                    if req_count % 10000000 == 0:
                        print(f'requests: {req_count}, dataset size: {len(keys)}')
                    ts = req_count * 86400 * 7 // 100000000
                    id_dig = hash(object_id) % 18446744073709551615
                    # Pack the data and write it to the binary file
                    binary_output.write(s.pack(ts, id_dig, 4000, -2))
                    keys.add(id_dig)
                except ValueError:
                    continue

if __name__ == "__main__":
    print("start")
    for i in range(1, 6):
        ############################# Choose one command ################################
        csv_to_binary(f'kvcache_traces_{i}.csv', 'kvcache_traces_1.dat') # traces for kv-1
        # csv_to_binary(f'kvcache_traces_{i}.csv', 'kvcache_traces_2.dat') # traces for kv-2
        # csv_to_binary(f'block_traces_{i}.csv', 'block_traces.dat') # traces for block
        #################################################################################
        print(f"file {i} ends")
    # traces for cdn:
    # csv_to_binary('reag0c01_20230315_20230322_0.2000.csv', 'cdn_traces.dat')
    # csv_to_binary('rnha0c01_20230315_20230322_0.8000.csv', 'kvcache_traces_1.dat')
    # csv_to_binary('rprn0c01_20230315_20230322_0.2000.csv', 'kvcache_traces_1.dat')
    print("end")
