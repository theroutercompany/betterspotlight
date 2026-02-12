if(NOT DEFINED BS_MODEL_SOURCE_DIR OR BS_MODEL_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "BS_MODEL_SOURCE_DIR is required")
endif()

if(NOT DEFINED BS_MODEL_DEST_DIR OR BS_MODEL_DEST_DIR STREQUAL "")
    message(FATAL_ERROR "BS_MODEL_DEST_DIR is required")
endif()

set(BS_MODEL_ONNX "${BS_MODEL_SOURCE_DIR}/bge-small-en-v1.5-int8.onnx")
set(BS_MODEL_VOCAB "${BS_MODEL_SOURCE_DIR}/vocab.txt")
set(BS_MODEL_MANIFEST "${BS_MODEL_SOURCE_DIR}/manifest.json")

if(NOT EXISTS "${BS_MODEL_ONNX}" OR NOT EXISTS "${BS_MODEL_VOCAB}" OR NOT EXISTS "${BS_MODEL_MANIFEST}")
    message(WARNING
        "Embedding model assets are missing in '${BS_MODEL_SOURCE_DIR}'. "
        "Run tools/fetch_embedding_models.sh to enable semantic search.")
    return()
endif()

file(MAKE_DIRECTORY "${BS_MODEL_DEST_DIR}")
file(COPY "${BS_MODEL_ONNX}" DESTINATION "${BS_MODEL_DEST_DIR}")
file(COPY "${BS_MODEL_VOCAB}" DESTINATION "${BS_MODEL_DEST_DIR}")
file(COPY "${BS_MODEL_MANIFEST}" DESTINATION "${BS_MODEL_DEST_DIR}")

message(STATUS "Synced embedding model assets to ${BS_MODEL_DEST_DIR}")
