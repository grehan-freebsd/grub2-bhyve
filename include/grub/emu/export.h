#ifdef __FreeBSD__
void EXPORT_FUNC (open) (void);
#else
void EXPORT_FUNC (open64) (void);
#endif
void EXPORT_FUNC (close) (void);
void EXPORT_FUNC (read) (void);
void EXPORT_FUNC (write) (void);
void EXPORT_FUNC (ioctl) (void);

