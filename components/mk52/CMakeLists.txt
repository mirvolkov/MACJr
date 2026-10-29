# Elektronika MK-52 calculator: the calculator core (mk52_core.c, no
# hardware dependencies) and the tty / keyboard app around it (mk52.c).
idf_component_register(
  SRCS "mk52.c" "mk52_core.c"
  INCLUDE_DIRS "."
  PRIV_INCLUDE_DIRS "private"
  REQUIRES display keyboard esp_timer
)
