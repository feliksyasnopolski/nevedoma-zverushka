# GNSS operational note

This note retains the bounded stock-TB330FU GNSS lifecycle observation before
obsolete local archaeology is removed. It is operational context, not proof of
current native-Linux GNSS ownership or a current hardware fix.

## Confirmed static/runtime facts

- The Android GNSS service is published as `android.hardware.gnss.IGnss/default`.
- The vendor path includes `mnld` (`/vendor/bin/mnld`) and its `mnld` stream
  socket, with the service declared as `user gps` and granted `WAKE_ALARM`.
- In one stock request trace, the start path was:

  ```text
  mnld gps_control_thread
    -> mnld_gps_start()
      -> linux_gps_init()
        -> mtk_gps_mnl_run(param1, param2)
          -> libmnl mtk_gps_run() worker
            -> mtk_gps_init(1, 0, param3)
  ```

- The trace observed `mtk_gps_mnl_run` returning status `18`; this is a
  lifecycle observation, not a claim that GNSS positioning succeeded.
- The stock initialization snapshot carried ChipID `0x6768`, with the related
  initialization fields `0x800001d0` and `0xffff6768`.
- The available GNSS evidence included NMEA/SV callback activity but did not
  establish a valid hardware fix or a native Linux NMEA policy owner.

The exact relative startup order of `mnld`, the LBS service, and the GNSS HAL
remains unverified. These observations must not be used to infer a fix from
service liveness, NMEA activity, or satellite reports alone.
