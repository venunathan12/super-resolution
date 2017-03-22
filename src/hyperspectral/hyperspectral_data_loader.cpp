#include "hyperspectral/hyperspectral_data_loader.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "image/image_data.h"
#include "util/config_reader.h"
#include "util/matrix_util.h"

#include "opencv2/core/core.hpp"

#include "glog/logging.h"

namespace super_resolution {
namespace hyperspectral {

constexpr char kMatlabTextDataDelimiter = ',';

struct HSIDataRange {
  int start_row = 0;
  int start_col = 0;
  int end_row = 0;
  int end_col = 0;
  int start_band = 0;
  int end_band = 0;
};

// Reverses the bytes of the given value (e.g. float). This is used to convert
// from the data's endian form into the machine's endian form when they are not
// matched up.
template <typename T>
T ReverseBytes(const T value) {
  T reversed_value;
  const unsigned char* original_bytes = (const unsigned char*)(&value);
  unsigned char* reversed_bytes  = (unsigned char*)(&reversed_value);
  const int num_bytes = sizeof(T);
  for (int i = 0; i < num_bytes; ++i) {
    reversed_bytes[i] = original_bytes[num_bytes - 1 - i];
  }
  return reversed_value;
}

template <typename T>
ImageData ReadBinaryFileBSQ(
    const std::string& hsi_file_path,
    const int num_data_rows,
    const int num_data_cols,
    const int num_data_bands,
    const long start_index,
    const bool reverse_bytes,
    const HSIDataRange& data_range) {

  std::ifstream input_file(hsi_file_path);
  CHECK(input_file.is_open())
      << "File '" << hsi_file_path << "' could not be opened for reading.";

  const int data_point_size = sizeof(T);
  long current_index = start_index;
  input_file.seekg(current_index * data_point_size);

  ImageData hsi_image;
  const cv::Size image_size(
      data_range.end_col - data_range.start_col,
      data_range.end_row - data_range.start_row);
  const long num_pixels = num_data_rows * num_data_cols;
  for (int band = data_range.start_band; band < data_range.end_band; ++band) {
    const long band_index = band * num_pixels;
    cv::Mat channel_image(image_size, util::kOpenCvMatrixType);
    for (int row = data_range.start_row; row < data_range.end_row; ++row) {
      const int channel_row = row - data_range.start_row;
      for (int col = data_range.start_col; col < data_range.end_col; ++col) {
        const int channel_col = col - data_range.start_col;
        const long pixel_index = row * num_data_cols + col;
        const long next_index = band_index + pixel_index;
        // Skip to next position if necessary.
        if (next_index > (current_index + 1)) {
          input_file.seekg(next_index * data_point_size);
        }
        T value;
        input_file.read(reinterpret_cast<char*>(&value), data_point_size);
        if (reverse_bytes) {
          value = ReverseBytes<T>(value);
        }
        channel_image.at<double>(channel_row, channel_col) =
            static_cast<double>(value);
        current_index = next_index;
      }
    }
    hsi_image.AddChannel(channel_image, DO_NOT_NORMALIZE_IMAGE);
  }
  input_file.close();
  return hsi_image;
}

ImageData ReadBinaryFile(
    const std::string& hsi_file_path,
    const HSIBinaryDataParameters& parameters,
    const HSIDataRange& data_range) {

  // Determine the machine endian. Union of memory means "unsigned int value"
  // and "unsigned char bytes" share the same memory.
  union UnsignedNumber {
    unsigned int value;
    unsigned char bytes[sizeof(unsigned int)];
  };
  // Set the value to unsigned 1, and check the byte array to see if it is in
  // big endian or little endian order. The left-most byte will be empty (zero)
  // if the machine is big endian.
  UnsignedNumber number;
  number.value = 1U;  // Unsigned int 1.
  const bool machine_big_endian = (number.bytes[0] != 1U);
  // If endians don't match, the bytes from the file have to be reversed.
  const bool reverse_bytes =
      (parameters.data_format.big_endian != machine_big_endian);

  // TODO: This may change, depending on interleave format and data type.
  return ReadBinaryFileBSQ<float>(
      hsi_file_path,
      parameters.num_data_rows,
      parameters.num_data_cols,
      parameters.num_data_bands,
      parameters.header_offset,
      reverse_bytes,
      data_range);
}

void HSIBinaryDataParameters::ReadHeaderFromFile(
    const std::string& header_file_path) {

  const std::unordered_map<std::string, std::string> key_value_map =
      util::ReadConfigurationFile(header_file_path, '=');
  for (const auto& key_value : key_value_map) {
    if (key_value.first == "interleave") {
      if (key_value.second == "bsq") {
        data_format.interleave = HSI_BINARY_INTERLEAVE_BSQ;
      } else {
        LOG(WARNING) << "Unknown/unsupported interleave format: "
                     << key_value.second << ". Using BSQ by default.";
      }
    } else if (key_value.first == "data type") {
      if (key_value.second == "4") {
        data_format.data_type = HSI_DATA_TYPE_FLOAT;
      } else {
        LOG(WARNING) << "Unknown/unsupported data type: "
                     << key_value.second << ". Using float by default.";
      }
    } else if (key_value.first == "byte order") {
      if (key_value.second == "1") {
        data_format.big_endian = true;
      } else {
        data_format.big_endian = false;
      }
    } else if (key_value.first == "header offset") {
      header_offset = std::atoi(key_value.second.c_str());
    } else if (key_value.first == "samples") {
      num_data_rows = std::atoi(key_value.second.c_str());
    } else if (key_value.first == "lines") {
      num_data_cols = std::atoi(key_value.second.c_str());
    } else if (key_value.first == "bands") {
      num_data_bands = std::atoi(key_value.second.c_str());
    } else {
      LOG(WARNING) << "Ignored header configuration entry: '"
                   << key_value.first << "'.";
    }
  }
}

void HyperspectralDataLoader::LoadImageFromENVIFile() {
  // TODO: Support for different interleaves and data types.
  const std::unordered_map<std::string, std::string> config_file_map =
      util::ReadConfigurationFile(file_path_, ' ');

  // Get the path of the binary data file.
  const std::string hsi_file_path =
      util::GetConfigValueOrDie(config_file_map, "file");

  // Get all of the necessary HSI file metadata.
  HSIBinaryDataParameters parameters;
  // Interleave format:
  const std::string interleave =
      util::GetConfigValueOrDie(config_file_map, "interleave");
  if (interleave == "bsq") {
    parameters.data_format.interleave = HSI_BINARY_INTERLEAVE_BSQ;
  } else {
    LOG(FATAL) << "Unsupported interleave format: '" << interleave << "'.";
  }
  // Data type:
  const std::string data_type =
      util::GetConfigValueOrDie(config_file_map, "data_type");
  if (data_type == "float") {
    parameters.data_format.data_type = HSI_DATA_TYPE_FLOAT;
  } else {
    LOG(FATAL) << "Unsupported data type: '" << data_type << "'.";
  }
  // Endian:
  const std::string big_endian =
      util::GetConfigValueOrDie(config_file_map, "big_endian");
  if (big_endian == "true") {
    parameters.data_format.big_endian = true;
  } else {
    parameters.data_format.big_endian = false;
  }
  // Header offset:
  const std::string header_offset =
      util::GetConfigValueOrDie(config_file_map, "header_offset");
  parameters.header_offset = std::atoi(header_offset.c_str());
  CHECK_GE(parameters.header_offset, 0)
      << "Header offset must be non-negative.";
  // Number of rows:
  const std::string num_data_rows =
      util::GetConfigValueOrDie(config_file_map, "num_data_rows");
  parameters.num_data_rows = std::atoi(num_data_rows.c_str());
  CHECK_GT(parameters.num_data_rows, 0)
      << "Number of data rows must be positive.";
  // Number of columns:
  const std::string num_data_cols =
      util::GetConfigValueOrDie(config_file_map, "num_data_cols");
  parameters.num_data_cols = std::atoi(num_data_cols.c_str());
  CHECK_GT(parameters.num_data_cols, 0)
      << "Number of data cols must be positive.";
  // Number of spectral bands:
  const std::string num_data_bands =
      util::GetConfigValueOrDie(config_file_map, "num_data_bands");
  parameters.num_data_bands = std::atoi(num_data_bands.c_str());
  CHECK_GT(parameters.num_data_bands, 0)
      << "Number of data bands must be positive.";

  // Now get the data range parameters.
  HSIDataRange data_range;
  // Start row:
  const std::string start_row_string =
      util::GetConfigValueOrDie(config_file_map, "start_row");
  data_range.start_row = std::atoi(start_row_string.c_str());
  CHECK_GE(data_range.start_row, 0) << "Start row index cannot be negative.";
  CHECK_LT(data_range.start_row, parameters.num_data_rows)
      << "Start row index is out of bounds.";
  // End row:
  const std::string end_row_string =
      util::GetConfigValueOrDie(config_file_map, "end_row");
  data_range.end_row = std::atoi(end_row_string.c_str());
  CHECK_GT(data_range.end_row, 0) << "End row index must be positive.";
  CHECK_LE(data_range.end_row, parameters.num_data_rows)
      << "End row index is out of bounds.";
  CHECK_GT(data_range.end_row - data_range.start_row, 0)
      << "Row range must be positive.";
  // Start column:
  const std::string start_col_string =
      util::GetConfigValueOrDie(config_file_map, "start_col");
  data_range.start_col = std::atoi(start_col_string.c_str());
  CHECK_GE(data_range.start_col, 0) << "Start column index cannot be negative.";
  CHECK_LT(data_range.start_col, parameters.num_data_cols)
      << "Start column index is out of bounds.";
  // End column:
  const std::string end_col_string =
      util::GetConfigValueOrDie(config_file_map, "end_col");
  data_range.end_col = std::atoi(end_col_string.c_str());
  CHECK_GT(data_range.end_col, 0) << "End column index must be positive.";
  CHECK_LE(data_range.end_col, parameters.num_data_cols)
      << "End column index is out of bounds.";
  CHECK_GT(data_range.end_col - data_range.start_col, 0)
      << "Column range must be positive.";
  // Start band:
  const std::string start_band_string =
      util::GetConfigValueOrDie(config_file_map, "start_band");
  data_range.start_band = std::atoi(start_band_string.c_str());
  CHECK_GE(data_range.start_band, 0) << "Start band index cannot be negative.";
  CHECK_LT(data_range.start_band, parameters.num_data_bands)
      << "Start band index is out of bounds.";
  // End band:
  const std::string end_band_string =
      util::GetConfigValueOrDie(config_file_map, "end_band");
  data_range.end_band = std::atoi(end_band_string.c_str());
  CHECK_GT(data_range.end_band, 0) << "End band index must be positive.";
  CHECK_LE(data_range.end_band, parameters.num_data_bands)
      << "End band index is out of bounds.";
  CHECK_GT(data_range.end_band - data_range.start_band, 0)
      << "Band range must be positive.";

  // And finally, read the data according to the parameters and range.
  hyperspectral_image_ = ReadBinaryFile(hsi_file_path, parameters, data_range);
}

ImageData HyperspectralDataLoader::GetImage() const {
  CHECK_GT(hyperspectral_image_.GetNumChannels(), 0)
      << "The hyperspectral image is empty. "
      << "Make sure to call LoadData() first.";
  return hyperspectral_image_;
}

void HyperspectralDataLoader::SaveImage(
    const ImageData& image,
    const HSIBinaryDataFormat& binary_data_format) const {

  // TODO: Implement.
}

}  // namespace hyperspectral
}  // namespace super_resolution
