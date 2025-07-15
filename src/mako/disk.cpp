#include <iostream>
#include <fstream>
#include <stdexcept>
#include <optional>

// g++ disk.cpp -o disk  -lboost_thread -lboost_system

class OnDiskIntArray {
public:
    OnDiskIntArray(const std::string& filename) : filename_(filename) {
        // Open the file in binary mode for both reading and writing
        file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_) {
            // If the file doesn't exist, create it
            file_.clear();
            file_.open(filename_, std::ios::out | std::ios::binary);
            file_.close();
            // Reopen in read/write mode
            file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
        }
    }

    ~OnDiskIntArray() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    int getByIndex(std::size_t index) {
        // Check if the requested index is in the buffer
        if (buffer_ && buffer_->index == index) {
            return buffer_->value;
        }

        // If not in buffer, read from disk
        std::size_t offset = index * sizeof(int);
        file_.seekg(offset, std::ios::beg);
        int value;
        file_.read(reinterpret_cast<char*>(&value), sizeof(int));
        if (file_.gcount() != sizeof(int)) {
            throw std::out_of_range("Index out of range");
        }

        // Update buffer
        buffer_ = Buffer{index, value};

        return value;
    }

    void append(int value) {
        file_.seekp(0, std::ios::end);
        file_.write(reinterpret_cast<const char*>(&value), sizeof(int));
        if (!file_) {
            throw std::ios_base::failure("Failed to write to file");
        }
    }

    void replace(std::size_t index, int value) {
        std::size_t offset = index * sizeof(int);
        file_.seekp(offset, std::ios::beg);
        file_.write(reinterpret_cast<const char*>(&value), sizeof(int));
        if (!file_) {
            throw std::ios_base::failure("Failed to write to file");
        }

        // Update buffer if it contains the replaced index
        if (buffer_ && buffer_->index == index) {
            buffer_->value = value;
        }
    }

    std::size_t size() {
        file_.seekg(0, std::ios::end);
        std::size_t fileSize = file_.tellg();
        if (fileSize == static_cast<std::size_t>(-1)) {
            throw std::ios_base::failure("Failed to determine file size");
        }
        return fileSize / sizeof(int);
    }

private:
    struct Buffer {
        std::size_t index;
        int value;
    };

    std::string filename_;
    std::fstream file_;
    std::optional<Buffer> buffer_;
};

int main() {
    try {
        OnDiskIntArray array("data.bin");

        // Append integers to the array
        array.append(10);
        array.append(20);
        array.append(30);

        // Retrieve and print integers by index
        std::cout << "Index 0: " << array.getByIndex(0) << std::endl;
        std::cout << "Index 1: " << array.getByIndex(1) << std::endl;
        std::cout << "Index 2: " << array.getByIndex(2) << std::endl;

        // Replace value at index 1
        array.replace(1, 25);
        std::cout << "Index 1 after replace: " << array.getByIndex(1) << std::endl;

        // Get and print the size of the array
        std::cout << "Size of array: " << array.size() << std::endl;
        std::cout << "Last Index: " << array.getByIndex(array.size()-1) << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}
