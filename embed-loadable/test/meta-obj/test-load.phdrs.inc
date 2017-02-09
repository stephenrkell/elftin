phdr0 6 PHDRS;
phdr1 3 ;
phdr2 1 PHDRS;
phdr3 1 ;
phdr4 2 ;
phdr5 4 ;
phdr6 0x6474e550 ;
phdr7 0x6474e551 ;
phdr8 0x6474e552 ;
changequote([,])dnl
define([expand_phdr], [ifelse([$1], [.gnu.version], [$1 $2 :phdr2], [$1], [.bss], [$1 $2 :phdr3], [$1], [.plt.got], [$1 $2 :phdr2], [$1], [.rodata], [$1 $2 :phdr2], [$1], [.dynamic], [$1 $2 :phdr3 :phdr4 :phdr8], [$1], [.dynstr], [$1 $2 :phdr2], [$1], [.plt], [$1 $2 :phdr2], [$1], [.rela.plt], [$1 $2 :phdr2], [$1], [.got.plt], [$1 $2 :phdr3], [$1], [.text], [$1 $2 :phdr2], [$1], [.note.gnu.build-id], [$1 $2 :phdr2 :phdr5], [$1], [.note.ABI-tag], [$1 $2 :phdr2 :phdr5], [$1], [.fini_array], [$1 $2 :phdr3 :phdr8], [$1], [.init], [$1 $2 :phdr2], [$1], [.interp], [$1 $2 :phdr1 :phdr2], [$1], [.gnu.hash], [$1 $2 :phdr2], [$1], [.rela.dyn], [$1 $2 :phdr2], [$1], [.jcr], [$1 $2 :phdr3 :phdr8], [$1], [.dynsym], [$1 $2 :phdr2], [$1], [.eh_frame], [$1 $2 :phdr2], [$1], [.fini], [$1 $2 :phdr2], [$1], [.eh_frame_hdr], [$1 $2 :phdr2 :phdr6], [$1], [.init_array], [$1 $2 :phdr3 :phdr8], [$1], [.got], [$1 $2 :phdr3 :phdr8], [$1], [.data], [$1 $2 :phdr3], [$1], [.gnu.version_r], [$1 $2 :phdr2], [$1 $2])])
