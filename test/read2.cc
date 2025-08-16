struct CompactPtr {
  // always points to 4 ints.
  int *elements;
};

void cutie_init (CompactPtr *self, void *(*malloc) (long long )) {
  self->elements = (int *) malloc (4 * sizeof (int ));
  return ;
}

int read2 (CompactPtr *ptr) {
  return ptr->elements[1]; 
}
