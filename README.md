# twain-acquire

A TWAIN host for intra-oral X-ray acquisition. It opens a TWAIN data source,
negotiates capabilities, acquires images into a layout of slots (a mount), and
exports them as DICOM or TIFF to the imaging software that reviews them.

It acquires and exports only. It contains no sensor driver and does no
diagnostic image processing.

## Safety

- The armed state is shown to the operator for as long as a sensor is armed.
- A staged set is delivered only after the operator confirms it, once, before a
  short expiry. Anything doubtful delivers nothing.

## License

MIT. See `LICENSE`.
