idf_component_register(
  SRCS "user_audio_bsp.c" 
  PRIV_REQUIRES 
  espressif__esp_codec_dev
  codec_board
  i2c_bsp
  INCLUDE_DIRS "./"
  EMBED_FILES "TF022.pcm")