if(NOT DEFINED BS_MODEL_SOURCE_DIR OR BS_MODEL_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "BS_MODEL_SOURCE_DIR is required")
endif()

if(NOT DEFINED BS_MODEL_DEST_DIR OR BS_MODEL_DEST_DIR STREQUAL "")
    message(FATAL_ERROR "BS_MODEL_DEST_DIR is required")
endif()

set(BS_MODEL_ONNX "${BS_MODEL_SOURCE_DIR}/bge-small-en-v1.5-int8.onnx")
set(BS_MODEL_VOCAB "${BS_MODEL_SOURCE_DIR}/vocab.txt")
set(BS_MODEL_MANIFEST "${BS_MODEL_SOURCE_DIR}/manifest.json")
set(BS_ONLINE_RANKER_ROOT "${BS_MODEL_SOURCE_DIR}/online-ranker-v1")
set(BS_ONLINE_RANKER_BOOTSTRAP_DIR "${BS_ONLINE_RANKER_ROOT}/bootstrap")
set(BS_ONLINE_RANKER_BOOTSTRAP_MODEL
    "${BS_ONLINE_RANKER_BOOTSTRAP_DIR}/online_ranker_v1.mlmodelc")
set(BS_ONLINE_RANKER_BOOTSTRAP_METADATA
    "${BS_ONLINE_RANKER_BOOTSTRAP_DIR}/metadata.json")

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

if(EXISTS "${BS_ONLINE_RANKER_BOOTSTRAP_MODEL}")
    file(COPY "${BS_ONLINE_RANKER_ROOT}" DESTINATION "${BS_MODEL_DEST_DIR}")
    if(NOT EXISTS "${BS_ONLINE_RANKER_BOOTSTRAP_METADATA}")
        message(WARNING
            "Online ranker bootstrap model exists without metadata.json at "
            "'${BS_ONLINE_RANKER_BOOTSTRAP_DIR}'. Learning engine will infer labels heuristically.")
    endif()
    message(STATUS "Synced online-ranker bootstrap assets to ${BS_MODEL_DEST_DIR}/online-ranker-v1")
else()
    message(STATUS "Online-ranker bootstrap artifacts not found in ${BS_ONLINE_RANKER_BOOTSTRAP_DIR}; CoreML online ranker will remain disabled until generated.")
endif()

message(STATUS "Synced embedding model assets to ${BS_MODEL_DEST_DIR}")
