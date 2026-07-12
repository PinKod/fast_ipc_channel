```mermaid
sequenceDiagram
    participant Main as Main Thread
    participant WriteThread as Write Thread
    participant Producer as Producer (SHM)
    participant File as Output File
    
    Main->>Main: Parse argv (shm_path, file_path)
    Main->>Main: Generate output_file_path
    Main->>File: open(output_file_path, O_RDWR | O_CREAT | O_TRUNC)
    Main->>Main: Loop: shm_open() until success
    Main->>Main: mmap(shm_fd) -> shm
    
    Main->>Main: Initialize thread_ctx<br/>(2 buffers, data pointers)
    Main->>Main: Set BUF_STATE_EMPTY for both buffers
    
    Main->>WriteThread: pthread_create(write_thread)
    activate WriteThread
    
    WriteThread->>WriteThread: pin_current_thread(cpu_core + 1)
    WriteThread->>WriteThread: current_buf = 0
    
    par Main Loop: Consume SHM Data
        loop Until STATE_EOF
            Main->>Main: Spin wait: STATE_EMPTY → STATE_FULL
            alt state == STATE_EOF
                Main->>Main: Break loop
            else state == STATE_FULL
                Main->>Main: Load wr_bytes from shm
                Main->>Main: memcpy(shm→data to buffer)
                Main->>Main: buffer_offset += size
                Main->>Main: Set shm→state = STATE_EMPTY
                
                alt buffer_offset + SHM_DATA_SIZE > LOCAL_BUF_SIZE
                    Main->>Main: Set lengths[current_buf]
                    Main->>WriteThread: Set states[current_buf] = BUF_STATE_FULL
                    Main->>Main: Toggle current_buf (0 ↔ 1)
                    Main->>Main: buffer_offset = 0
                    Main->>Main: Spin wait: other buffer = BUF_STATE_EMPTY
                end
            end
        end
    and Write Thread Loop: Write to File
        loop Until BUF_STATE_DONE
            WriteThread->>WriteThread: Spin wait: state == BUF_STATE_EMPTY
            alt state == BUF_STATE_DONE
                WriteThread->>WriteThread: Break loop
            else state == BUF_STATE_FULL
                WriteThread->>WriteThread: size = lengths[current_buf]
                WriteThread->>File: write(fd, data, size)
                WriteThread->>WriteThread: Update written_bytes
                WriteThread->>Main: Set states[current_buf] = BUF_STATE_EMPTY
                WriteThread->>WriteThread: Toggle current_buf (0 ↔ 1)
            end
        end
    end
    
    Main->>Main: Final buffer: if buffer_offset > 0
    Main->>Main: Set lengths & BUF_STATE_FULL
    Main->>Main: Spin wait: other buffer = BUF_STATE_EMPTY
    
    Main->>WriteThread: Set states[current_buf] = BUF_STATE_DONE
    deactivate WriteThread
    Main->>WriteThread: pthread_join()
    
    Main->>Main: Free allocated buffers
    Main->>Producer: atomic_store(shm→state = STATE_EOF)
    Main->>Main: munmap(shm)
    Main->>File: close(fd)
    Main->>Main: close(shm_fd)
    Main->>Producer: shm_unlink(shm_path)
    Main->>Main: Return 0```